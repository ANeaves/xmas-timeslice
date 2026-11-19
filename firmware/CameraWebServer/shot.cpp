#include "shot.h"
#include "config.h"
#include "net.h"
#include "settings.h"
#include "board_config.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static SemaphoreHandle_t g_stateMutex = NULL;  // serialises arm / disarm / capture
static SemaphoreHandle_t g_shotMutex = NULL;   // protects the shot buffer
static TaskHandle_t g_taskHandle = NULL;
static TaskHandle_t g_armTaskHandle = NULL;
static volatile bool g_armRequested = false;  // set by cameraArmAsync(), cleared once the arm task is done

static volatile CamState g_state = CAM_DISARMED;
static volatile int g_users = 0;  // in-flight stock HTTP handlers using the camera
static volatile uint32_t g_lastActivityMs = 0;
static volatile uint32_t g_missed = 0;
static volatile int64_t g_triggerUs = 0;
static volatile int64_t g_lastIsrUs = 0;

static uint8_t *g_shotBuf = NULL;
static ShotInfo g_shot = {};
static char g_lastError[64] = "";

// Exposure / gain / white-balance lock
static bool g_lockRequested = false;
static bool g_lockValid = false;  // g_lockVals holds values read from an OV5640

// ---------------------------------------------------------------------------
// Sensor power
// ---------------------------------------------------------------------------
static void sensorPowerOff() {
#if PWDN_GPIO_NUM >= 0
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);  // PWDN high = sensor powered down
#endif
}

static void setError(const char *msg) {
  strlcpy(g_lastError, msg, sizeof(g_lastError));
  Serial.printf("shot: %s\n", msg);
}

// ---------------------------------------------------------------------------
// Exposure / white balance lock (OV5640 register level, so the exact values
// survive a power-down and every camera can be locked to the same look)
// ---------------------------------------------------------------------------
struct LockReg {
  uint16_t reg;
  uint8_t mask;
  int val;
};
static LockReg g_lockRegs[] = {
  { 0x3500, 0x0F, 0 }, { 0x3501, 0xFF, 0 }, { 0x3502, 0xFF, 0 },  // exposure
  { 0x350A, 0x03, 0 }, { 0x350B, 0xFF, 0 },                       // gain
  { 0x3400, 0x0F, 0 }, { 0x3401, 0xFF, 0 },                       // AWB gain R
  { 0x3402, 0x0F, 0 }, { 0x3403, 0xFF, 0 },                       // AWB gain G
  { 0x3404, 0x0F, 0 }, { 0x3405, 0xFF, 0 },                       // AWB gain B
};
static const size_t kLockRegCount = sizeof(g_lockRegs) / sizeof(g_lockRegs[0]);

static bool readLockRegs(sensor_t *s) {
  if (!s || s->id.PID != OV5640_PID) {
    return false;
  }
  for (size_t i = 0; i < kLockRegCount; i++) {
    int v = s->get_reg(s, g_lockRegs[i].reg, g_lockRegs[i].mask);
    if (v < 0) {
      return false;
    }
    g_lockRegs[i].val = v;
  }
  return true;
}

static void setAuto(sensor_t *s, bool enable) {
  s->set_exposure_ctrl(s, enable);
  s->set_gain_ctrl(s, enable);
  s->set_whitebal(s, enable);
}

static void applyManual(sensor_t *s, bool writeValues) {
  setAuto(s, false);
  if (writeValues) {
    for (size_t i = 0; i < kLockRegCount; i++) {
      s->set_reg(s, g_lockRegs[i].reg, g_lockRegs[i].mask, g_lockRegs[i].val);
    }
  }
}

static void warmUp(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      break;
    }
    esp_camera_fb_return(fb);
  }
}

// ---------------------------------------------------------------------------
// Arm / disarm
// ---------------------------------------------------------------------------
void cameraTouch() {
  g_lastActivityMs = millis();
}

void cameraAcquire() {
  __atomic_fetch_add(&g_users, 1, __ATOMIC_SEQ_CST);
}

void cameraRelease() {
  __atomic_fetch_sub(&g_users, 1, __ATOMIC_SEQ_CST);
}

esp_err_t cameraArm() {
  xSemaphoreTake(g_stateMutex, portMAX_DELAY);
  if (g_state == CAM_ARMED || g_state == CAM_CAPTURING) {
    cameraTouch();
    xSemaphoreGive(g_stateMutex);
    return ESP_OK;
  }

  g_state = CAM_ARMING;
  netSetLowPower(false);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = CAMERA_XCLK_HZ;
  config.frame_size = (framesize_t)g_cfg.framesize;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = g_cfg.quality;
  config.fb_count = 1;  // a single 5MP-class frame buffer; two do not fit comfortably in PSRAM
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    char msg[48];
    snprintf(msg, sizeof(msg), "camera init failed: 0x%x", err);
    setError(msg);
    sensorPowerOff();
    netSetLowPower(true);
    g_state = CAM_DISARMED;
    xSemaphoreGive(g_stateMutex);
    return err;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, g_cfg.vflip);
    s->set_hmirror(s, g_cfg.hmirror);

    if (g_lockRequested && g_lockValid) {
      applyManual(s, true);
      warmUp(400);  // just flush the first frames
    } else {
      warmUp(g_cfg.warmupMs);  // let auto exposure / white balance settle
      if (g_lockRequested) {
        g_lockValid = readLockRegs(s);
        applyManual(s, g_lockValid);
      }
    }
  }

  g_lastError[0] = 0;
  g_state = CAM_ARMED;
  cameraTouch();
  xSemaphoreGive(g_stateMutex);
  Serial.println("camera: armed");
  return ESP_OK;
}

static void armTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (g_armRequested) {  // false if a disarm cancelled the request before we got here
      cameraArm();
      g_armRequested = false;
    }
  }
}

bool cameraArmAsync() {
  if (cameraIsArmed()) {
    cameraTouch();
    return false;
  }
  if (!g_armRequested) {
    g_lastError[0] = 0;  // so a failure of this arm is not confused with an older error
  }
  g_armRequested = true;
  xTaskNotifyGive(g_armTaskHandle);
  return true;
}

bool cameraDisarm(uint32_t waitMs) {
  g_armRequested = false;                        // cancel an arm that has not started yet
  xSemaphoreTake(g_stateMutex, portMAX_DELAY);  // waits for an in-flight arm or capture
  if (g_state == CAM_DISARMED) {
    xSemaphoreGive(g_stateMutex);
    return true;
  }

  g_state = CAM_POWERING_DOWN;  // new stock handlers now see "not armed" and bail out
  uint32_t t0 = millis();
  while (g_users > 0 && millis() - t0 < waitMs) {
    delay(20);
  }
  if (g_users > 0) {
    g_state = CAM_ARMED;  // someone is still streaming; leave the camera running
    xSemaphoreGive(g_stateMutex);
    return false;
  }

  esp_camera_deinit();
  sensorPowerOff();
  netSetLowPower(true);
  g_state = CAM_DISARMED;
  xSemaphoreGive(g_stateMutex);
  Serial.println("camera: powered down");
  return true;
}

bool cameraAutoDisarmEnabled() {
  return g_cfg.idleMs > 0;
}

void cameraIdleCheck() {
  if (cameraAutoDisarmEnabled() && g_state == CAM_ARMED && g_users == 0 && millis() - g_lastActivityMs > g_cfg.idleMs) {
    Serial.println("camera: idle timeout");
    cameraDisarm(0);
  }
}

CamState cameraState() {
  // A queued arm reports ARMING straight away, before the arm task has taken the state mutex.
  if (g_state == CAM_DISARMED && g_armRequested) {
    return CAM_ARMING;
  }
  return g_state;
}

const char *cameraStateName() {
  switch (cameraState()) {
    case CAM_DISARMED:      return "disarmed";
    case CAM_ARMING:        return "arming";
    case CAM_ARMED:         return "armed";
    case CAM_CAPTURING:     return "capturing";
    case CAM_POWERING_DOWN: return "powering_down";
  }
  return "unknown";
}

bool cameraIsArmed() {
  return g_state == CAM_ARMED || g_state == CAM_CAPTURING;
}

uint32_t cameraIdleRemainingMs() {
  if (g_state != CAM_ARMED || !cameraAutoDisarmEnabled()) {
    return 0;
  }
  uint32_t idle = millis() - g_lastActivityMs;
  return idle >= g_cfg.idleMs ? 0 : g_cfg.idleMs - idle;
}

bool cameraSetLock(bool lock) {
  xSemaphoreTake(g_stateMutex, portMAX_DELAY);
  bool ok = false;
  if (g_state == CAM_ARMED) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
      if (lock) {
        g_lockValid = readLockRegs(s);
        applyManual(s, g_lockValid);
        g_lockRequested = true;
      } else {
        g_lockRequested = false;
        g_lockValid = false;
        setAuto(s, true);
      }
      ok = true;
    }
  }
  xSemaphoreGive(g_stateMutex);
  return ok;
}

bool cameraIsLocked() {
  return g_lockRequested;
}

// ---------------------------------------------------------------------------
// Shot capture
// ---------------------------------------------------------------------------
static void doCapture(int64_t triggerUs) {
  uint32_t t0 = millis();

  // With one frame buffer the driver keeps the last completed frame until it is
  // read, so the first frame we get may predate the trigger. Read-and-return it
  // to discard it, then the next frame we get is exposed after the trigger.
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
  }
  fb = esp_camera_fb_get();
  if (!fb) {
    setError("capture failed (no frame)");
    return;
  }
  if (fb->format != PIXFORMAT_JPEG || fb->len > SHOT_BUFFER_SIZE) {
    setError(fb->format != PIXFORMAT_JPEG ? "frame is not JPEG" : "frame larger than SHOT_BUFFER_SIZE");
    esp_camera_fb_return(fb);
    return;
  }
  if (xSemaphoreTake(g_shotMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    setError("shot buffer busy, shot dropped");
    esp_camera_fb_return(fb);
    return;
  }
  memcpy(g_shotBuf, fb->buf, fb->len);
  g_shot.size = fb->len;
  g_shot.width = fb->width;
  g_shot.height = fb->height;
  g_shot.triggerUs = triggerUs;
  g_shot.doneUs = esp_timer_get_time();
  g_shot.captureMs = millis() - t0;
  g_shot.ready = true;
  g_shot.id++;
  xSemaphoreGive(g_shotMutex);
  esp_camera_fb_return(fb);
  Serial.printf("shot #%u: %u bytes, %ums\n", (unsigned)g_shot.id, (unsigned)g_shot.size, (unsigned)g_shot.captureMs);
}

static void shotTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int64_t triggerUs = g_triggerUs;

    // Arming / disarming in progress, or a previous capture still running: ignore.
    if (xSemaphoreTake(g_stateMutex, 0) != pdTRUE) {
      g_missed++;
      continue;
    }
    if (g_state != CAM_ARMED) {
      g_missed++;
      xSemaphoreGive(g_stateMutex);
      continue;
    }

    g_state = CAM_CAPTURING;
    doCapture(triggerUs);
    g_state = CAM_ARMED;
    cameraTouch();
    xSemaphoreGive(g_stateMutex);

    ulTaskNotifyTake(pdTRUE, 0);  // drop triggers that arrived mid-capture
  }
}

static void IRAM_ATTR triggerIsr() {
  int64_t now = esp_timer_get_time();
  if (now - g_lastIsrUs < (int64_t)g_cfg.debounceMs * 1000) {
    return;
  }
  g_lastIsrUs = now;
  g_triggerUs = now;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(g_taskHandle, &woken);
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

bool shotTrigger() {
  if (g_state != CAM_ARMED) {
    return false;
  }
  g_triggerUs = esp_timer_get_time();
  xTaskNotifyGive(g_taskHandle);
  return true;
}

void shotBegin() {
  sensorPowerOff();  // sensor stays off until /arm

  g_stateMutex = xSemaphoreCreateMutex();
  g_shotMutex = xSemaphoreCreateMutex();

  if (!psramFound()) {
    setError("PSRAM not found - enable PSRAM in the board settings");
  } else {
    g_shotBuf = (uint8_t *)heap_caps_malloc(SHOT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!g_shotBuf) {
      setError("could not allocate shot buffer");
    }
  }

  xTaskCreatePinnedToCore(shotTask, "shot", 8192, NULL, 5, &g_taskHandle, 1);
  // Arming takes seconds (sensor init + warm-up), so it runs here instead of in the HTTP task.
  xTaskCreatePinnedToCore(armTask, "arm", 8192, NULL, 4, &g_armTaskHandle, tskNO_AFFINITY);

  pinMode(TRIGGER_GPIO_NUM, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(TRIGGER_GPIO_NUM), triggerIsr, RISING);
  Serial.printf("shot: trigger on GPIO%d, shot buffer %u KB\n", TRIGGER_GPIO_NUM, (unsigned)(SHOT_BUFFER_SIZE / 1024));
}

// ---------------------------------------------------------------------------
// Shot access
// ---------------------------------------------------------------------------
ShotInfo shotGetInfo() {
  return g_shot;  // unlocked copy: fine for status display
}

bool shotAcquire(uint32_t timeoutMs) {
  return xSemaphoreTake(g_shotMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void shotRelease() {
  xSemaphoreGive(g_shotMutex);
}

const uint8_t *shotData() {
  return g_shotBuf;
}

uint32_t shotMissedTriggers() {
  return g_missed;
}

const char *shotLastError() {
  return g_lastError;
}
