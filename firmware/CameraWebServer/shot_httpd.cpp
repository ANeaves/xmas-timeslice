#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "config.h"
#include "net.h"
#include "settings.h"
#include "http_util.h"
#include "shot.h"
#include "webui.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static esp_err_t send_status(httpd_req_t *req, const char *httpStatus = "200 OK", const char *error = NULL) {
  ShotInfo info = shotGetInfo();
  char err[96];
  json_escape_copy(err, sizeof(err), error ? error : shotLastError());
  String mac = WiFi.macAddress();
  String ip = netIp().toString();

  char body[768];
  snprintf(
    body, sizeof(body),
    "{\"ok\":%s,\"configured\":%s,\"cam_id\":%d,\"mac\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,"
    "\"state\":\"%s\",\"locked\":%s,\"auto_disarm\":%s,\"idle_ms\":%u,"
    "\"shot_id\":%u,\"ready\":%s,\"size\":%u,\"width\":%u,\"height\":%u,"
    "\"trigger_us\":%lld,\"latency_ms\":%d,\"capture_ms\":%u,\"missed\":%u,"
    "\"uptime_ms\":%u,\"error\":\"%s\"}",
    error ? "false" : "true", g_cfg.configured ? "true" : "false", g_cfg.camId, mac.c_str(), ip.c_str(), netModeName(), netMode() == NET_STA ? WiFi.RSSI() : 0, cameraStateName(),
    cameraIsLocked() ? "true" : "false", cameraAutoDisarmEnabled() ? "true" : "false", (unsigned)cameraIdleRemainingMs(), (unsigned)info.id, info.ready ? "true" : "false", (unsigned)info.size,
    (unsigned)info.width, (unsigned)info.height, (long long)info.triggerUs, (int)((info.doneUs - info.triggerUs) / 1000), (unsigned)info.captureMs,
    (unsigned)shotMissedTriggers(), (unsigned)millis(), err
  );
  return send_json(req, httpStatus, body);
}

// ---------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------
static esp_err_t console_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, WEBUI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_json_handler(httpd_req_t *req) {
  return send_status(req);
}

// POST /arm: returns at once. 200 if already armed, 202 if arming started (poll /shot_status
// until state is "armed", or "disarmed" with an error if the sensor failed to start).
static esp_err_t arm_handler(httpd_req_t *req) {
  if (!cameraArmAsync()) {
    return send_status(req);
  }
  return send_status(req, "202 Accepted");
}

static esp_err_t disarm_handler(httpd_req_t *req) {
  if (!cameraDisarm(2000)) {
    return send_status(req, "409 Conflict", "camera busy (close the stream in the full UI)");
  }
  return send_status(req);
}

static esp_err_t trigger_handler(httpd_req_t *req) {
  if (!shotTrigger()) {
    return send_status(req, "409 Conflict", "camera is not armed (or is busy)");
  }
  return send_status(req);
}

static esp_err_t lock_handler(httpd_req_t *req) {
  if (!cameraSetLock(true)) {
    return send_status(req, "409 Conflict", "camera must be armed to lock exposure");
  }
  return send_status(req);
}

static esp_err_t unlock_handler(httpd_req_t *req) {
  if (!cameraSetLock(false)) {
    return send_status(req, "409 Conflict", "camera must be armed to unlock exposure");
  }
  return send_status(req);
}

static esp_err_t shot_handler(httpd_req_t *req) {
  if (!shotAcquire(3000)) {
    return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"shot buffer busy\"}");
  }
  ShotInfo info = shotGetInfo();
  if (!info.ready) {
    shotRelease();
    return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"no shot taken yet\"}");
  }

  char id[16];
  char ts[32];
  snprintf(id, sizeof(id), "%u", (unsigned)info.id);
  snprintf(ts, sizeof(ts), "%lld", (long long)info.triggerUs);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Shot-Id", id);
  httpd_resp_set_hdr(req, "X-Trigger-Us", ts);
  esp_err_t res = httpd_resp_send(req, (const char *)shotData(), info.size);
  shotRelease();
  cameraTouch();
  return res;
}

// POST /update   header "X-Auth: <admin password>", body = raw firmware .bin
// optional query ?md5=<32 hex chars>
static esp_err_t update_handler(httpd_req_t *req) {
  if (!authOk(req)) {
    return send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"bad or missing X-Auth\"}");
  }
  size_t total = req->content_len;
  if (total == 0) {
    return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"empty body\"}");
  }
  if (!cameraDisarm(2000)) {
    return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"camera busy, close the stream first\"}");
  }

  char query[64];
  char md5[40];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && httpd_query_key_value(query, "md5", md5, sizeof(md5)) == ESP_OK) {
    Update.setMD5(md5);
  }

  if (!Update.begin(total, U_FLASH)) {
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", Update.errorString());
    return send_json(req, "500 Internal Server Error", body);
  }

  const size_t CHUNK = 2048;
  uint8_t *buf = (uint8_t *)malloc(CHUNK);
  if (!buf) {
    Update.abort();
    return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"out of memory\"}");
  }

  size_t received = 0;
  bool failed = false;
  while (received < total) {
    int n = httpd_req_recv(req, (char *)buf, min(CHUNK, total - received));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (n <= 0) {
      failed = true;
      break;
    }
    if (Update.write(buf, n) != (size_t)n) {
      failed = true;
      break;
    }
    received += n;
  }
  free(buf);

  if (failed || !Update.end(true)) {
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", failed && !Update.hasError() ? "transfer failed" : Update.errorString());
    Update.abort();
    return send_json(req, "500 Internal Server Error", body);
  }

  send_json(req, "200 OK", "{\"ok\":true,\"msg\":\"update written, rebooting\"}");
  Serial.println("OTA: update complete, rebooting");
  delay(500);
  ESP.restart();
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------
static void add_route(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  u.user_ctx = NULL;
  httpd_register_uri_handler(server, &u);
}

void registerShotHandlers(httpd_handle_t server) {
  add_route(server, "/", HTTP_GET, console_handler);
  add_route(server, "/shot_status", HTTP_GET, status_json_handler);
  // Actions that change state are POST (no body needed); reads are GET.
  add_route(server, "/arm", HTTP_POST, arm_handler);
  add_route(server, "/disarm", HTTP_POST, disarm_handler);
  add_route(server, "/trigger", HTTP_POST, trigger_handler);
  add_route(server, "/lock", HTTP_POST, lock_handler);
  add_route(server, "/unlock", HTTP_POST, unlock_handler);
  add_route(server, "/shot", HTTP_GET, shot_handler);
  add_route(server, "/update", HTTP_POST, update_handler);
  registerSetupHandlers(server);
}
