#include <Arduino.h>
#include <WiFi.h>
#include "esp_ota_ops.h"

#include "config.h"
#include "board_config.h"
#include "net.h"
#include "settings.h"
#include "shot.h"

void startCameraServer();
void setupLedFlash();

// Keep a freshly OTA-flashed image "pending" until we have confirmed it works (see setup()).
// Only has an effect if the Arduino core was built with app rollback enabled.
extern "C" bool verifyRollbackLater() {
  return true;
}

void setup() {
  settingsLoad();                       // saved settings from flash (factory defaults if never configured)
  setCpuFrequencyMhz(g_cfg.cpuMhz);     // lower clock = less heat; nothing here is compute heavy

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.printf("ESP32 timeslice camera #%d (%s)\n", g_cfg.camId, g_cfg.configured ? "configured" : "NOT configured - setup mode");

  // Sensor stays powered down until /arm. Also allocates the shot buffer and arms the trigger pin.
  shotBegin();

#if defined(LED_GPIO_NUM)
  setupLedFlash();  // duty 0 => flash LED off
#endif

  // Unconfigured: setup AP. Configured: join the Pi's AP (static IP), or fall back to our own AP.
  netBegin();

  startCameraServer();

  // The new image booted, has network and is serving HTTP: confirm it so it is not rolled back.
  esp_ota_mark_app_valid_cancel_rollback();

  Serial.printf("Ready: http://%s/  (mode: %s)\n", netIp().toString().c_str(), netModeName());
}

void loop() {
  netLoop();
  cameraIdleCheck();
  delay(250);
}
