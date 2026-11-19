#include "net.h"
#include "config.h"
#include "settings.h"
#include "esp_mac.h"

static NetMode g_mode = NET_NONE;
static bool g_setupMode = false;  // AP is the factory "unconfigured" setup AP
static bool g_lowPower = true;
static uint32_t g_lastStaTry = 0;
static uint32_t g_lostSince = 0;

static void applyPowerSave() {
  if (g_mode == NET_STA) {
    WiFi.setSleep(g_lowPower ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  }
}

static IPAddress camIp() {
  IPAddress ip;
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%s.0", g_cfg.ipPrefix);
  ip.fromString(tmp);
  ip[3] = g_cfg.hostBase + g_cfg.camId;
  return ip;
}

static bool tryStation(uint32_t timeoutMs) {
  char host[16];
  snprintf(host, sizeof(host), "espcam-%d", g_cfg.camId);
  WiFi.setHostname(host);
  WiFi.mode(WIFI_STA);

  IPAddress ip = camIp();
  IPAddress gw, mask;
  gw.fromString(g_cfg.gateway);
  mask.fromString(g_cfg.netmask);
  WiFi.config(ip, gw, mask, gw);
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_cfg.ssid, g_cfg.wifiPass);
  WiFi.setTxPower((wifi_power_t)g_cfg.txPower);

  Serial.printf("WiFi: joining '%s' as %s (%s)", g_cfg.ssid, host, ip.toString().c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    g_mode = NET_STA;
    g_setupMode = false;
    g_lostSince = 0;
    applyPowerSave();
    Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

// setup = true: board has no saved settings (SSID uses the MAC since there is no ID yet).
static void startAp(bool setup) {
  char ssid[28];
  if (setup) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, sizeof(ssid), "espcam-setup-%02X%02X", mac[4], mac[5]);
  } else {
    snprintf(ssid, sizeof(ssid), "espcam-%d-setup", g_cfg.camId);
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  IPAddress apIp(FALLBACK_AP_IP_BYTES);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid, g_cfg.apPass, g_cfg.apChannel, 0, 4);
  WiFi.setTxPower((wifi_power_t)g_cfg.txPower);

  g_mode = NET_AP;
  g_setupMode = setup;
  g_lastStaTry = millis();
  Serial.printf("WiFi: AP '%s' up at %s (%s)\n", ssid, apIp.toString().c_str(), setup ? "unconfigured, setup mode" : "Pi network not available");
}

void netBegin() {
  WiFi.persistent(false);
  if (!g_cfg.configured) {
    startAp(true);  // nothing saved yet: do not look for the Pi, wait for configuration
    return;
  }
  if (!tryStation(STA_TIMEOUT_MS)) {
    startAp(false);
  }
}

void netLoop() {
  uint32_t now = millis();
  if (g_mode == NET_STA) {
    if (WiFi.status() == WL_CONNECTED) {
      g_lostSince = 0;
    } else if (g_lostSince == 0) {
      g_lostSince = now;
    } else if (now - g_lostSince > STA_TIMEOUT_MS) {
      Serial.println("WiFi: lost the Pi network, starting fallback AP");
      startAp(false);
    }
  } else if (g_mode == NET_AP && !g_setupMode) {
    // Only probe for the Pi when nobody is using the fallback AP (probing drops it briefly).
    if (WiFi.softAPgetStationNum() == 0 && now - g_lastStaTry > STA_RETRY_INTERVAL_MS) {
      g_lastStaTry = now;
      WiFi.softAPdisconnect(true);
      if (!tryStation(STA_TIMEOUT_MS)) {
        startAp(false);
      }
    }
  }
}

NetMode netMode() {
  return g_mode;
}

bool netIsSetupMode() {
  return g_setupMode;
}

const char *netModeName() {
  switch (g_mode) {
    case NET_STA: return "sta";
    case NET_AP:  return "ap";
    default:      return "none";
  }
}

IPAddress netIp() {
  return g_mode == NET_AP ? WiFi.softAPIP() : WiFi.localIP();
}

void netSetLowPower(bool lowPower) {
  g_lowPower = lowPower;
  applyPowerSave();
}
