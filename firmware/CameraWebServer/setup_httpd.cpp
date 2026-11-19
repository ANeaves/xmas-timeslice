#include <Arduino.h>
#include "esp_http_server.h"
#include "http_util.h"
#include "settings.h"
#include "shot.h"
#include "setup_ui.h"

static esp_err_t setup_page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /config  (also /config.json; passwords are never returned)
static esp_err_t config_json_handler(httpd_req_t *req) {
  char ssid[72];
  json_escape_copy(ssid, sizeof(ssid), g_cfg.ssid);

  String body;
  body.reserve(1200);
  char buf[400];
  snprintf(
    buf, sizeof(buf),
    "{\"ok\":true,\"configured\":%s,\"cam_id\":%u,\"ssid\":\"%s\",\"wifi_pass_set\":%s,\"ip_prefix\":\"%s\",\"host_base\":%u,"
    "\"gateway\":\"%s\",\"netmask\":\"%s\",\"ap_channel\":%u,\"tx_power\":%d,\"cpu_mhz\":%u,\"debounce_ms\":%u,\"idle_s\":%u,"
    "\"warmup_ms\":%u,\"framesize\":%d,\"quality\":%u,\"vflip\":%s,\"hmirror\":%s,",
    g_cfg.configured ? "true" : "false", g_cfg.camId, ssid, g_cfg.wifiPass[0] ? "true" : "false", g_cfg.ipPrefix, g_cfg.hostBase, g_cfg.gateway,
    g_cfg.netmask, g_cfg.apChannel, g_cfg.txPower, g_cfg.cpuMhz, g_cfg.debounceMs, (unsigned)(g_cfg.idleMs / 1000), g_cfg.warmupMs, g_cfg.framesize,
    g_cfg.quality, g_cfg.vflip ? "true" : "false", g_cfg.hmirror ? "true" : "false"
  );
  body += buf;

  body += "\"framesizes\":[";
  for (size_t i = 0; i < kFrameSizeOptionCount; i++) {
    snprintf(buf, sizeof(buf), "%s{\"v\":%d,\"n\":\"%s\"}", i ? "," : "", kFrameSizeOptions[i].value, kFrameSizeOptions[i].name);
    body += buf;
  }
  body += "],\"tx_powers\":[";
  for (size_t i = 0; i < kTxPowerOptionCount; i++) {
    snprintf(buf, sizeof(buf), "%s{\"v\":%d,\"n\":\"%s\"}", i ? "," : "", kTxPowerOptions[i].value, kTxPowerOptions[i].name);
    body += buf;
  }
  body += "]}";
  return send_json(req, "200 OK", body.c_str());
}

static void restart_soon() {
  delay(500);  // let the HTTP response flush
  ESP.restart();
}

// PATCH /config   header X-Auth: <admin password>   body: form-urlencoded fields (see settingsApplyForm)
// PATCH because only the fields sent are changed; the rest keep their saved values.
static esp_err_t config_patch_handler(httpd_req_t *req) {
  if (!authOk(req)) {
    return send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"wrong admin password\"}");
  }
  size_t len = req->content_len;
  if (len == 0 || len > 1023) {
    return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"body missing or too large\"}");
  }
  char body[1024];
  size_t got = 0;
  while (got < len) {
    int n = httpd_req_recv(req, body + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (n <= 0) {
      return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"receive failed\"}");
    }
    got += n;
  }
  body[got] = 0;

  char err[96];
  if (!settingsApplyForm(body, err, sizeof(err))) {
    char esc[128];
    char out[192];
    json_escape_copy(esc, sizeof(esc), err);
    snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", esc);
    return send_json(req, "400 Bad Request", out);
  }

  send_json(req, "200 OK", "{\"ok\":true,\"msg\":\"saved, rebooting\"}");
  Serial.println("settings: saved, rebooting");
  restart_soon();
  return ESP_OK;
}

// DELETE /config   header X-Auth: <admin password>   erases saved settings (factory reset)
static esp_err_t config_delete_handler(httpd_req_t *req) {
  if (!authOk(req)) {
    return send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"wrong admin password\"}");
  }
  settingsFactoryReset();
  send_json(req, "200 OK", "{\"ok\":true,\"msg\":\"settings erased, rebooting into setup mode\"}");
  Serial.println("settings: factory reset, rebooting");
  restart_soon();
  return ESP_OK;
}

static void add_route(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  u.user_ctx = NULL;
  httpd_register_uri_handler(server, &u);
}

void registerSetupHandlers(httpd_handle_t server) {
  add_route(server, "/setup", HTTP_GET, setup_page_handler);
  add_route(server, "/config", HTTP_GET, config_json_handler);
  add_route(server, "/config.json", HTTP_GET, config_json_handler);
  add_route(server, "/config", HTTP_PATCH, config_patch_handler);
  add_route(server, "/config", HTTP_DELETE, config_delete_handler);
}
