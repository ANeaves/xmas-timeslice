#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// Runtime settings, persisted in flash (NVS, namespace "cfg").
// config.h only provides the factory defaults used when nothing has been saved.
struct Settings {
  bool configured;  // false until the first save from /setup => board starts in setup-AP mode

  // identity / network
  uint8_t camId;      // 0-7
  char ssid[33];      // the Pi's AP
  char wifiPass[65];
  char ipPrefix[16];  // first three octets, e.g. "192.168.4"
  uint8_t hostBase;   // camera IP = ipPrefix.(hostBase + camId)
  char gateway[16];   // the Pi
  char netmask[16];

  // fallback / setup AP
  uint8_t apChannel;
  char apPass[65];

  // access control for /update and PATCH / DELETE /config
  char adminPass[65];

  // power / heat
  int txPower;  // wifi_power_t
  uint8_t cpuMhz;
  uint32_t idleMs;  // auto power-down after this long idle, 0 = never

  // trigger / camera
  uint16_t debounceMs;
  uint16_t warmupMs;
  int framesize;  // framesize_t
  uint8_t quality;
  bool vflip;
  bool hmirror;
};

extern Settings g_cfg;

struct SettingOption {
  int value;
  const char *name;
};
extern const SettingOption kFrameSizeOptions[];
extern const size_t kFrameSizeOptionCount;
extern const SettingOption kTxPowerOptions[];
extern const size_t kTxPowerOptionCount;

void settingsLoad();            // load from flash (defaults if never saved)
bool settingsSave();            // persist g_cfg
void settingsFactoryReset();    // erase flash settings and restore defaults (board becomes unconfigured)

// Parses an application/x-www-form-urlencoded body, validates it, applies it to g_cfg and saves.
// Only fields present are changed; blank/absent password fields keep their old value.
bool settingsApplyForm(const char *body, char *err, size_t errLen);

#endif  // SETTINGS_H
