#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <Arduino.h>
#include "esp_http_server.h"
#include "settings.h"

static inline esp_err_t send_json(httpd_req_t *req, const char *status, const char *body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static inline void json_escape_copy(char *dst, size_t dstLen, const char *src) {
  size_t o = 0;
  for (; *src && o + 2 < dstLen; src++) {
    if (*src == '"' || *src == '\\') {
      dst[o++] = '\\';
    }
    dst[o++] = ((unsigned char)*src >= 0x20) ? *src : ' ';
  }
  dst[o] = 0;
}

// Header "X-Auth: <admin password>" (protects POST /update, PATCH /config, DELETE /config)
static inline bool authOk(httpd_req_t *req) {
  char auth[72] = "";
  return httpd_req_get_hdr_value_str(req, "X-Auth", auth, sizeof(auth)) == ESP_OK && strcmp(auth, g_cfg.adminPass) == 0;
}

#endif  // HTTP_UTIL_H
