#include "settings.h"
#include "config.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include <Preferences.h>
#include <WiFi.h>

Settings g_cfg;

const SettingOption kFrameSizeOptions[] = {
  { FRAMESIZE_VGA, "VGA 640x480" },
  { FRAMESIZE_SVGA, "SVGA 800x600" },
  { FRAMESIZE_XGA, "XGA 1024x768" },
  { FRAMESIZE_HD, "HD 1280x720" },
  { FRAMESIZE_SXGA, "SXGA 1280x1024" },
  { FRAMESIZE_UXGA, "UXGA 1600x1200" },
  { FRAMESIZE_FHD, "FHD 1920x1080" },
  { FRAMESIZE_QXGA, "QXGA 2048x1536" },
  { FRAMESIZE_QSXGA, "QSXGA 2560x1920 (5MP class)" },
};
const size_t kFrameSizeOptionCount = sizeof(kFrameSizeOptions) / sizeof(kFrameSizeOptions[0]);

const SettingOption kTxPowerOptions[] = {
  { WIFI_POWER_2dBm, "2 dBm" },   { WIFI_POWER_5dBm, "5 dBm" },     { WIFI_POWER_8_5dBm, "8.5 dBm" },   { WIFI_POWER_11dBm, "11 dBm" },
  { WIFI_POWER_13dBm, "13 dBm" }, { WIFI_POWER_15dBm, "15 dBm" },   { WIFI_POWER_17dBm, "17 dBm" },     { WIFI_POWER_19_5dBm, "19.5 dBm (max)" },
};
const size_t kTxPowerOptionCount = sizeof(kTxPowerOptions) / sizeof(kTxPowerOptions[0]);

static bool inOptions(const SettingOption *opts, size_t n, int v) {
  for (size_t i = 0; i < n; i++) {
    if (opts[i].value == v) {
      return true;
    }
  }
  return false;
}

static void setDefaults() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.configured = false;
  g_cfg.camId = CAM_ID;
  strlcpy(g_cfg.ssid, WIFI_SSID, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.wifiPass, WIFI_PASSWORD, sizeof(g_cfg.wifiPass));
  snprintf(g_cfg.ipPrefix, sizeof(g_cfg.ipPrefix), "%d.%d.%d", NET_PREFIX_BYTES);
  g_cfg.hostBase = CAM_IP_HOST_BASE;
  snprintf(g_cfg.gateway, sizeof(g_cfg.gateway), "%d.%d.%d.%d", NET_GATEWAY_BYTES);
  snprintf(g_cfg.netmask, sizeof(g_cfg.netmask), "%d.%d.%d.%d", NET_SUBNET_BYTES);
  g_cfg.apChannel = FALLBACK_AP_CHANNEL;
  strlcpy(g_cfg.apPass, FALLBACK_AP_PASSWORD, sizeof(g_cfg.apPass));
  strlcpy(g_cfg.adminPass, OTA_PASSWORD, sizeof(g_cfg.adminPass));
  g_cfg.txPower = (int)WIFI_TX_POWER;
  g_cfg.cpuMhz = CPU_FREQ_MHZ;
  g_cfg.idleMs = IDLE_DISARM_MS;
  g_cfg.debounceMs = TRIGGER_DEBOUNCE_MS;
  g_cfg.warmupMs = ARM_WARMUP_MS;
  g_cfg.framesize = (int)SHOT_FRAMESIZE;
  g_cfg.quality = SHOT_JPEG_QUALITY;
  g_cfg.vflip = SHOT_VFLIP;
  g_cfg.hmirror = SHOT_HMIRROR;
}

void settingsLoad() {
  setDefaults();
  Preferences p;
  if (!p.begin("cfg", false)) {
    Serial.println("settings: NVS open failed, using defaults");
    return;
  }
  if (!p.getBool("cfgd", false)) {
    p.end();
    Serial.println("settings: nothing saved yet (unconfigured)");
    return;
  }
  g_cfg.configured = true;
  g_cfg.camId = p.getUChar("cam_id", g_cfg.camId);
  if (!p.isKey("id0")) {
    // Saved by firmware that numbered cameras 1-8: shift to 0-7 once and remember that we did.
    g_cfg.camId = g_cfg.camId > 0 ? g_cfg.camId - 1 : 0;
    p.putUChar("cam_id", g_cfg.camId);
    p.putBool("id0", true);
    Serial.printf("settings: camera ID migrated to 0-based numbering (now %u)\n", g_cfg.camId);
  }
  p.getString("ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
  p.getString("wifi_pass", g_cfg.wifiPass, sizeof(g_cfg.wifiPass));
  p.getString("ip_prefix", g_cfg.ipPrefix, sizeof(g_cfg.ipPrefix));
  g_cfg.hostBase = p.getUChar("host_base", g_cfg.hostBase);
  p.getString("gateway", g_cfg.gateway, sizeof(g_cfg.gateway));
  p.getString("netmask", g_cfg.netmask, sizeof(g_cfg.netmask));
  g_cfg.apChannel = p.getUChar("ap_channel", g_cfg.apChannel);
  p.getString("ap_pass", g_cfg.apPass, sizeof(g_cfg.apPass));
  p.getString("admin_pass", g_cfg.adminPass, sizeof(g_cfg.adminPass));
  g_cfg.txPower = p.getInt("tx_power", g_cfg.txPower);
  g_cfg.cpuMhz = p.getUChar("cpu_mhz", g_cfg.cpuMhz);
  g_cfg.idleMs = p.getUInt("idle_ms", g_cfg.idleMs);
  g_cfg.debounceMs = p.getUShort("debounce_ms", g_cfg.debounceMs);
  g_cfg.warmupMs = p.getUShort("warmup_ms", g_cfg.warmupMs);
  g_cfg.framesize = p.getInt("framesize", g_cfg.framesize);
  g_cfg.quality = p.getUChar("quality", g_cfg.quality);
  g_cfg.vflip = p.getBool("vflip", g_cfg.vflip);
  g_cfg.hmirror = p.getBool("hmirror", g_cfg.hmirror);
  p.end();
}

bool settingsSave() {
  Preferences p;
  if (!p.begin("cfg", false)) {
    return false;
  }
  p.putUChar("cam_id", g_cfg.camId);
  p.putBool("id0", true);  // cam_id is 0-based (see settingsLoad)
  p.putString("ssid", g_cfg.ssid);
  p.putString("wifi_pass", g_cfg.wifiPass);
  p.putString("ip_prefix", g_cfg.ipPrefix);
  p.putUChar("host_base", g_cfg.hostBase);
  p.putString("gateway", g_cfg.gateway);
  p.putString("netmask", g_cfg.netmask);
  p.putUChar("ap_channel", g_cfg.apChannel);
  p.putString("ap_pass", g_cfg.apPass);
  p.putString("admin_pass", g_cfg.adminPass);
  p.putInt("tx_power", g_cfg.txPower);
  p.putUChar("cpu_mhz", g_cfg.cpuMhz);
  p.putUInt("idle_ms", g_cfg.idleMs);
  p.putUShort("debounce_ms", g_cfg.debounceMs);
  p.putUShort("warmup_ms", g_cfg.warmupMs);
  p.putInt("framesize", g_cfg.framesize);
  p.putUChar("quality", g_cfg.quality);
  p.putBool("vflip", g_cfg.vflip);
  p.putBool("hmirror", g_cfg.hmirror);
  p.putBool("cfgd", g_cfg.configured);  // written last: a half-finished save stays "unconfigured"
  p.end();
  return true;
}

void settingsFactoryReset() {
  Preferences p;
  if (p.begin("cfg", false)) {
    p.clear();
    p.end();
  }
  setDefaults();
}

// ---------------------------------------------------------------------------
// Form parsing
// ---------------------------------------------------------------------------
static void urlDecode(char *s) {
  char *o = s;
  while (*s) {
    if (*s == '+') {
      *o++ = ' ';
      s++;
    } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char hex[3] = { s[1], s[2], 0 };
      *o++ = (char)strtol(hex, NULL, 16);
      s += 3;
    } else {
      *o++ = *s++;
    }
  }
  *o = 0;
}

static bool getField(const char *body, const char *key, char *out, size_t outLen) {
  char raw[200];
  if (httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) {
    return false;
  }
  urlDecode(raw);
  strlcpy(out, raw, outLen);
  return true;
}

static bool validIp(const char *s) {
  IPAddress ip;
  return ip.fromString(s);
}

bool settingsApplyForm(const char *body, char *err, size_t errLen) {
  Settings n = g_cfg;
  char v[80];
#define FAIL(...)                       \
  do {                                  \
    snprintf(err, errLen, __VA_ARGS__); \
    return false;                       \
  } while (0)

  if (getField(body, "cam_id", v, sizeof(v))) {
    int x = atoi(v);
    if (v[0] < '0' || v[0] > '9' || x < 0 || x > 7) FAIL("cam_id must be 0-7");
    n.camId = x;
  }
  if (getField(body, "ssid", v, sizeof(v))) {
    if (strlen(v) < 1 || strlen(v) > 32) FAIL("ssid must be 1-32 characters");
    strlcpy(n.ssid, v, sizeof(n.ssid));
  }
  if (getField(body, "wifi_pass", v, sizeof(v)) && v[0]) {
    if (strlen(v) < 8 || strlen(v) > 64) FAIL("wifi password must be 8-64 characters");
    strlcpy(n.wifiPass, v, sizeof(n.wifiPass));
  }
  if (getField(body, "ip_prefix", v, sizeof(v))) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%s.0", v);
    if (!validIp(tmp)) FAIL("ip_prefix must look like 192.168.4");
    strlcpy(n.ipPrefix, v, sizeof(n.ipPrefix));
  }
  if (getField(body, "host_base", v, sizeof(v))) {
    int x = atoi(v);
    if (x < 1 || x > 247) FAIL("host_base must be 1-247");
    n.hostBase = x;
  }
  if (getField(body, "gateway", v, sizeof(v))) {
    if (!validIp(v)) FAIL("gateway is not a valid IP address");
    strlcpy(n.gateway, v, sizeof(n.gateway));
  }
  if (getField(body, "netmask", v, sizeof(v))) {
    if (!validIp(v)) FAIL("netmask is not a valid IP address");
    strlcpy(n.netmask, v, sizeof(n.netmask));
  }
  if (getField(body, "ap_channel", v, sizeof(v))) {
    int x = atoi(v);
    if (x < 1 || x > 13) FAIL("ap_channel must be 1-13");
    n.apChannel = x;
  }
  if (getField(body, "ap_pass", v, sizeof(v)) && v[0]) {
    if (strlen(v) < 8 || strlen(v) > 63) FAIL("fallback AP password must be 8-63 characters");
    strlcpy(n.apPass, v, sizeof(n.apPass));
  }
  if (getField(body, "new_admin_pass", v, sizeof(v)) && v[0]) {
    if (strlen(v) > 63) FAIL("admin password must be at most 63 characters");
    strlcpy(n.adminPass, v, sizeof(n.adminPass));
  }
  if (getField(body, "tx_power", v, sizeof(v))) {
    int x = atoi(v);
    if (!inOptions(kTxPowerOptions, kTxPowerOptionCount, x)) FAIL("unsupported tx_power");
    n.txPower = x;
  }
  if (getField(body, "cpu_mhz", v, sizeof(v))) {
    int x = atoi(v);
    if (x != 80 && x != 160 && x != 240) FAIL("cpu_mhz must be 80, 160 or 240");
    n.cpuMhz = x;
  }
  if (getField(body, "debounce_ms", v, sizeof(v))) {
    int x = atoi(v);
    if (x < 5 || x > 1000) FAIL("debounce_ms must be 5-1000");
    n.debounceMs = x;
  }
  if (getField(body, "idle_s", v, sizeof(v))) {
    int x = atoi(v);
    if (v[0] < '0' || v[0] > '9' || (x != 0 && (x < 5 || x > 3600))) FAIL("idle_s must be 0 (never) or 5-3600");
    n.idleMs = (uint32_t)x * 1000;
  }
  if (getField(body, "warmup_ms", v, sizeof(v))) {
    int x = atoi(v);
    if (x < 0 || x > 10000) FAIL("warmup_ms must be 0-10000");
    n.warmupMs = x;
  }
  if (getField(body, "framesize", v, sizeof(v))) {
    int x = atoi(v);
    if (!inOptions(kFrameSizeOptions, kFrameSizeOptionCount, x)) FAIL("unsupported framesize");
    n.framesize = x;
  }
  if (getField(body, "quality", v, sizeof(v))) {
    int x = atoi(v);
    if (x < 0 || x > 63) FAIL("quality must be 0-63");
    n.quality = x;
  }
  if (getField(body, "vflip", v, sizeof(v))) {
    n.vflip = atoi(v) != 0;
  }
  if (getField(body, "hmirror", v, sizeof(v))) {
    n.hmirror = atoi(v) != 0;
  }
  if ((int)n.hostBase + (int)n.camId > 254) FAIL("host_base + cam_id must be at most 254");
#undef FAIL

  n.configured = true;
  Settings old = g_cfg;
  g_cfg = n;
  if (!settingsSave()) {
    g_cfg = old;
    snprintf(err, errLen, "could not write settings to flash");
    return false;
  }
  return true;
}
