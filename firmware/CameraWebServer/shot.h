#ifndef SHOT_H
#define SHOT_H

#include <Arduino.h>
#include "esp_http_server.h"

// Camera lifecycle:
//   DISARMED  - sensor powered down (PWDN high), minimal heat. Triggers are ignored.
//   ARMING    - arm requested / sensor powering up and settling (runs in the background).
//   ARMED     - sensor streaming, waiting for the trigger pin (or /trigger).
//   CAPTURING - a trigger fired; grabbing the frame.
enum CamState { CAM_DISARMED, CAM_ARMING, CAM_ARMED, CAM_CAPTURING, CAM_POWERING_DOWN };

struct ShotInfo {
  uint32_t id;  // increments with every stored shot, 0 = none yet
  size_t size;
  uint16_t width;
  uint16_t height;
  int64_t triggerUs;  // esp_timer time of the trigger
  int64_t doneUs;     // esp_timer time the image was stored
  uint32_t captureMs;
  bool ready;
};

// Call once from setup(): powers the sensor down, allocates the shot buffer,
// starts the capture task and attaches the trigger interrupt.
void shotBegin();

// Power the sensor up. Blocks ~1-3 s, so HTTP uses cameraArmAsync() instead.
esp_err_t cameraArm();
// Queues an arm on a background task and returns immediately. Returns false if the camera
// was already armed (idle timer reset), true if an arm was started or is already in progress.
// Progress: cameraState() is ARMING until it becomes ARMED, or DISARMED with shotLastError() set.
bool cameraArmAsync();
// waitMs: how long to wait for open stream/capture requests to finish. Returns false if still busy.
bool cameraDisarm(uint32_t waitMs);
// Powers down after g_cfg.idleMs without activity (never if idleMs is 0). Call from loop().
void cameraIdleCheck();

CamState cameraState();
const char *cameraStateName();
bool cameraIsArmed();  // ARMED or CAPTURING
bool cameraAutoDisarmEnabled();
uint32_t cameraIdleRemainingMs();  // 0 unless ARMED with auto power-down enabled

// Exposure / gain / white balance lock (full persistence across arm cycles on OV5640).
bool cameraSetLock(bool lock);
bool cameraIsLocked();

// Software trigger (same path as the GPIO). False if not armed or busy.
bool shotTrigger();

// Last shot. shotAcquire() must be held while reading shotData().
ShotInfo shotGetInfo();
bool shotAcquire(uint32_t timeoutMs);
void shotRelease();
const uint8_t *shotData();
uint32_t shotMissedTriggers();
const char *shotLastError();

// Stock camera handlers (/stream, /control, ...) hold a CamGuard so the camera
// cannot be powered down underneath them and so they count as activity.
void cameraAcquire();
void cameraRelease();
void cameraTouch();
struct CamGuard {
  CamGuard() {
    cameraAcquire();
    cameraTouch();
  }
  ~CamGuard() {
    cameraRelease();
  }
};

// Registers the test console, GET /shot_status, /shot and POST /arm, /disarm, /trigger, /lock, /unlock, /update.
void registerShotHandlers(httpd_handle_t server);
// Registers /setup, GET /config(.json), PATCH /config, DELETE /config (also called by registerShotHandlers).
void registerSetupHandlers(httpd_handle_t server);

#endif  // SHOT_H
