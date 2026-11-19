# ESP32 Timeslice Camera: Raspberry Pi API

How the Raspberry Pi talks to the 8 ESP32-CAM (OV5640) boards: network layout, the hardware trigger, the HTTP API and the recommended capture sequence.

Defaults below are the factory defaults from [CameraWebServer/config.h](CameraWebServer/config.h). Every board runs the **same firmware**, and each board's real values (ID, WiFi, IP scheme, passwords, ...) are saved in its flash through the setup page (§2.1), so the numbers here are the defaults, not constants.

---

## 1. Overview

Each camera is **powered down** (sensor off, low heat) until the Pi **arms** it over HTTP. Once armed, a **rising edge on a GPIO pin** takes a single photo at the same instant on every camera. The photo is stored in the camera's RAM, and the Pi then **downloads** it over HTTP and **disarms** the camera.

```
Pi                               each ESP32-CAM
 │  POST /arm  ──────────────────▶  returns at once (202, state=arming)
 │                                   powers up sensor, settles exposure in the background (~2-4 s)
 │  GET /shot_status (poll)  ────▶  wait until state=armed, remember shot_id
 │  GPIO pulse  ═══════════════════▶  ISR → capture task → JPEG kept in PSRAM
 │  GET /shot_status (poll)  ────▶  wait until shot_id increases
 │  GET /shot  ──────────────────▶  JPEG bytes
 │  POST /disarm  ───────────────▶  power sensor down
```

- **HTTP** is for control and image transfer. Its latency does not matter.
- **The GPIO line** is the only thing that sets *when* the photo is taken, so it is what synchronises the cameras.

## 2. Network

| Item | Value |
|---|---|
| Radio | 2.4 GHz only (the ESP32 has no 5 GHz). The Pi AP must be 2.4 GHz, preferably on a fixed channel (1, 6 or 11) |
| SSID / password | Saved in the camera (setup page, §2.1). Factory default `WIFI_SSID` / `WIFI_PASSWORD` in `config.h` |
| Pi (gateway) | `192.168.4.1` |
| Camera N (0-7) | `192.168.4.(10 + N)`, so camera 0 = `192.168.4.10` … camera 7 = `192.168.4.17` |
| Netmask | `255.255.255.0` |
| Addressing | Static, derived from the saved camera ID and IP settings. No DHCP and no mDNS needed |
| HTTP port | 80 (the stock MJPEG stream uses 81, see §6) |

Requirements for the Pi's AP:
- Use the subnet above, or change the IP prefix and gateway on each camera's setup page.
- Keep `192.168.4.10`-`.17` outside the DHCP pool, since the cameras do not use DHCP.

### 2.1 First-time setup and fallback AP

A camera only creates its own access point **when it needs one**. It never runs an AP while it is happily connected to the Pi.

| Camera state | Behaviour at boot |
|---|---|
| **Unconfigured** (fresh flash, or after "reset to factory defaults") | Does not look for the Pi. Starts the **setup AP** `espcam-setup-<last 4 hex of MAC>` and stays there |
| **Configured** | Joins the Pi's AP with its static IP |
| **Configured, Pi's AP not found within 15 s** (or lost for more than 15 s) | Starts the **fallback AP** `espcam-<ID>-setup`. While nobody is connected to it, retries the Pi's network every 60 s |

Both APs use the AP password (factory default `FALLBACK_AP_PASSWORD` in `config.h`) and serve the whole API, including the browser pages, at **`http://192.168.4.1/`**. Join the AP with a phone or laptop and open:

- `http://192.168.4.1/setup`: settings page. Set the camera ID, the Pi's WiFi name and password, and the other settings, then press **Save & reboot**.
- `http://192.168.4.1/`: test console (arm, trigger, view the shot).

Settings are saved in flash and survive reboots and power loss. To the Pi, a camera in setup or fallback mode simply does not answer at its static IP.

`GET /shot_status` reports `"mode":"sta"` (on the Pi's AP) or `"mode":"ap"` (setup or fallback), and `"configured": false` for a board that has never been set up.

**Pi-side flow for a new board:** flash the generic firmware, join `espcam-setup-XXXX` from a laptop, set ID and WiFi, save. The board reboots and joins the Pi's AP at `192.168.4.(10+ID)`.

## 3. Hardware trigger

| Item | Value |
|---|---|
| ESP32 pin | GPIO13 (`TRIGGER_GPIO_NUM`) on every camera |
| Logic | 3.3 V, **rising edge** triggers. Internal pull-down, so the line idles low |
| Pulse | Drive high for at least ~5 ms, then low. Only the rising edge matters |
| Debounce | A second edge within **50 ms** of the previous one is ignored |
| Wiring | One Pi GPIO fanned out to all 8 cameras with a ~330 Ω-1 kΩ series resistor on each. **All grounds must be common** (Pi ground and every camera's ground) |

When a pulse is **accepted** or **ignored**:

| Camera state | Pulse result |
|---|---|
| `armed` | Photo taken |
| `disarmed`, `arming`, `powering_down` | Ignored, `missed` counter increments |
| `capturing` | Ignored, `missed` counter increments (a photo is already being taken) |

The `missed` counter is how the Pi can detect that a camera did not take a photo (see §8).

## 4. Camera states

`state` in `/shot_status`:

| State | Meaning | Heat |
|---|---|---|
| `disarmed` | Sensor powered down. Triggers ignored. This is the boot state | low |
| `arming` | Arm requested; sensor powering up and settling in the background (after `POST /arm`) | high |
| `armed` | Sensor streaming, waiting for a trigger | high |
| `capturing` | Photo being taken and copied (typically well under a second) | high |
| `powering_down` | Transient, during `POST /disarm` | - |

**Auto power-down:** an armed camera powers itself down after **30 s** (`IDLE_DISARM_MS`, setting `idle_s`) with no activity. Activity is any of: `POST /arm`, an accepted trigger, a `/shot` download, or a stock camera request (§6). **`/shot_status` polling does not count as activity.** If you arm early and wait longer than 30 s, call `POST /arm` again (it is idempotent and resets the timer) before triggering.

**Set `idle_s` to `0` to turn auto power-down off.** The camera then stays armed until `POST /disarm`, a reboot or an OTA update, so remember to disarm it yourself to limit heat. `/shot_status` reports `"auto_disarm": false` in this mode.

## 5. HTTP API

Base URL: `http://192.168.4.<10+N>/`

Reads are `GET`. Actions that change the camera's state (`/arm`, `/disarm`, `/trigger`, `/lock`, `/unlock`) are `POST` with no body. Settings use `GET` / `PATCH` / `DELETE /config` (§5.9). Calling an action with `GET` returns `405 Method Not Allowed`.

The control endpoints reply with the same status JSON (§5.1). Handle **one request at a time per camera**: the camera's web server is a single task, so a long request (a 1 MB `/shot`) delays other requests to the same camera until it finishes. Different cameras can be driven in parallel. `POST /arm` returns immediately, so it can simply be sent to each camera in turn.

### 5.1 `GET /shot_status`

Non-blocking, side-effect free, safe to poll (about once per 100-250 ms is fine).

```json
{
  "ok": true,
  "configured": true,
  "cam_id": 3,
  "mac": "AA:BB:CC:DD:EE:FF",
  "ip": "192.168.4.13",
  "mode": "sta",
  "rssi": -48,
  "state": "armed",
  "locked": true,
  "auto_disarm": true,
  "idle_ms": 24100,
  "shot_id": 7,
  "ready": true,
  "size": 412345,
  "width": 2048,
  "height": 1536,
  "trigger_us": 123456789,
  "latency_ms": 143,
  "capture_ms": 141,
  "missed": 0,
  "uptime_ms": 987654,
  "error": ""
}
```

| Field | Meaning |
|---|---|
| `ok` | `true` for a normal status reply. `false` on an error reply (see §5.8) |
| `configured` | `false` if the board has never been set up (it is in the setup AP) |
| `cam_id`, `mac`, `ip` | Identity of the camera, for checking that the right board is at the right address |
| `mode` | `sta` (Pi's AP) or `ap` (fallback) |
| `rssi` | WiFi signal in dBm. `0` in `ap` mode |
| `state` | See §4 |
| `locked` | `true` if exposure / gain / white balance lock is requested (see `/lock`) |
| `auto_disarm` | `false` if auto power-down is turned off (`idle_s` = 0) |
| `idle_ms` | Time left before auto power-down. `0` unless `armed` with `auto_disarm` on |
| `shot_id` | Counter that **increases by 1 for every stored shot**. `0` = no shot since boot |
| `ready` | `true` once at least one shot is stored |
| `size`, `width`, `height` | Size in bytes and pixel dimensions of the **stored** shot |
| `trigger_us` | The camera's own microsecond clock at the trigger (not comparable between cameras) |
| `latency_ms` | Trigger to image stored, on the camera's clock. Includes waiting for a fresh frame |
| `capture_ms` | Time spent inside the capture itself |
| `missed` | Triggers received while the camera could not take them |
| `uptime_ms` | Milliseconds since boot. **If it goes backwards, the camera rebooted** |
| `error` | Last error message, `""` if none. It is cleared when a new arm starts |

### 5.2 `POST /arm`

Starts powering up the sensor and **returns immediately**. The sensor initialises and exposure / white balance settle in the background, which takes roughly 2-4 s (1.5 s warm-up, or about 0.4 s if exposure is locked, plus sensor initialisation).

- `202 Accepted` with `"state":"arming"`: arming has started (or was already in progress).
- `200` with `"state":"armed"`: it was already armed. The idle timer is reset.
- Then poll `GET /shot_status` until `state` is:
  - `armed`: ready.
  - `disarmed` with a non-empty `error` (such as `camera init failed: 0x...`): the sensor failed to start. Retry once, then check power and the ribbon cable.
- Give up if it is still `arming` after about 10 s.
- So you can send `POST /arm` to all cameras one after another without waiting, then poll them all.
- A `POST /disarm` sent while arming waits for the arm to finish, then powers down.

### 5.3 `POST /trigger`

**Software trigger**, the same code path as the GPIO. Meant for testing and as a fallback, because it is not synchronised across cameras.

- Returns **immediately**, before the photo is taken. Poll `/shot_status` for `shot_id` to increase.
- `200` if accepted. `409` (`"camera is not armed (or is busy)"`) otherwise.

### 5.4 `GET /shot_status` → `GET /shot`

`GET /shot` returns the stored photo.

- `200`, `Content-Type: image/jpeg`, body is a complete JPEG (about 0.2-1 MB at QXGA).
- Response headers:
  - `X-Shot-Id`: the `shot_id` of this image. **Check it matches the id you expect.**
  - `X-Trigger-Us`: the camera's microsecond clock at the trigger.
- `404` if no shot has been taken since boot. `503` if the buffer is busy (retry shortly).
- **Works while the camera is `disarmed`.** The image stays in RAM until a new shot overwrites it or the camera reboots.
- **There is a single buffer per camera.** A new trigger overwrites the previous shot, so download before triggering again.
- Validate the download: starts with `FF D8`, ends with `FF D9`, length equals `size` from `/shot_status`.

### 5.5 `POST /disarm`

Powers the sensor down. Returns `200` with `"state":"disarmed"`. Returns `409` if a stream or other stock-UI request is still open on that camera. Close it and retry. The stored shot is kept.

### 5.6 `POST /lock` and `POST /unlock`

Freeze (or release) auto exposure, auto gain and auto white balance. **Strongly recommended for bullet-time**, so that all 8 cameras use fixed settings and the sequence doesn't flicker between frames.

- The camera must be `armed`, otherwise `409`.
- Typical use: arm all cameras, wait a moment under the final lighting, call `/lock` on all of them once.
- On OV5640 the exact exposure, gain and white-balance register values are saved and **re-applied on every later `/arm`**, so the camera does not need to re-settle. The lock lasts until `/unlock` or a reboot.
- If the sensor is not an OV5640, auto control is simply switched off for the current armed session.

### 5.7 `POST /update`: firmware update (OTA)

```
curl -X POST \
     -H "X-Auth: <admin password>" \
     --data-binary @firmware.bin \
     "http://192.168.4.13/update"
```

- Body is the raw `.bin` (the application image, not the bootloader or partition table). Optional `?md5=<32 hex chars>` verifies the upload.
- Header `X-Auth` must equal the camera's **admin password** (saved setting, factory default `OTA_PASSWORD` in `config.h`). Otherwise `401`.
- The camera is disarmed first (`409` if a stream is still open).
- `200` `{"ok":true,"msg":"update written, rebooting"}`, then the camera **reboots**. Wait roughly 10-15 s, then check `/shot_status` (`uptime_ms` should be small).
- `500` with an error string on failure. The old firmware keeps running.
- Update cameras **one or a few at a time**. Updating all 8 at once over one 2.4 GHz AP is slow and unreliable.
- The partition table changed with this firmware, so the **first** flash of each board must be over serial. OTA works after that.
- Settings saved in flash are **kept** across an OTA update.

### 5.8 Error replies

Control endpoints reply with the status JSON and `"ok": false`. The `error` field says why. (A failed arm is not an error reply, because `POST /arm` returns before the sensor starts; see §5.2.)

| HTTP | When |
|---|---|
| `409` | Not armed / busy (`/trigger`, `/lock`, `/unlock`), or the camera is in use (`/disarm`, `/update`) |
| `500` | OTA error |
| `400` | `PATCH /config` with an invalid value (`error` says which) |
| `401` | `POST /update`, `PATCH /config` or `DELETE /config` with a bad or missing `X-Auth` |
| `405` | Wrong method, for example `GET /arm` |
| `404` | `/shot` before any shot exists |
| `503` | `/shot` buffer busy |

### 5.9 Configuration: `/setup`, `GET` / `PATCH` / `DELETE /config`

| Endpoint | Purpose |
|---|---|
| `GET /setup` | Browser settings page |
| `GET /config` | Current settings as JSON (passwords are never returned). Also lists the allowed resolutions and TX power values. `GET /config.json` is the same |
| `PATCH /config` | Change settings and reboot. Header `X-Auth: <admin password>`. Body is `application/x-www-form-urlencoded` |
| `DELETE /config` | Erase all saved settings (factory reset) and reboot into setup mode. Header `X-Auth: <admin password>` |

`PATCH /config` only changes the fields you send (that is why it is `PATCH` and not `PUT`, which would mean replacing every setting). A blank or missing password field keeps the old password. Fields:

| Field | Range / meaning |
|---|---|
| `cam_id` | 0-7 |
| `ssid`, `wifi_pass` | The Pi's AP. Password 8-64 characters |
| `ip_prefix`, `host_base`, `gateway`, `netmask` | Camera IP = `ip_prefix.(host_base + cam_id)`, e.g. `192.168.4` and `10` |
| `ap_pass`, `ap_channel` | Setup / fallback AP password (8-63 characters) and channel (1-13) |
| `new_admin_pass` | New admin password (also protects `/update`) |
| `framesize`, `quality`, `vflip`, `hmirror` | Camera output. `framesize` must be one of the values listed in `/config.json` |
| `warmup_ms`, `debounce_ms`, `idle_s` | Settle time after arming, trigger debounce, auto power-down delay in seconds (5-3600, or `0` = never) |
| `tx_power`, `cpu_mhz` | WiFi TX power (value from `/config.json`), CPU speed 80, 160 or 240 |

```
curl -X PATCH -H "X-Auth: <admin password>" \
     -d "cam_id=3" -d "idle_s=20" \
     http://192.168.4.13/config

curl -X DELETE -H "X-Auth: <admin password>" http://192.168.4.13/config   # factory reset
```

Replies: `200` `{"ok":true,"msg":"saved, rebooting"}` (the camera then reboots; wait ~10-15 s), `400` with an `error` message for an invalid value, `401` for a wrong admin password. Changing the ID or IP settings moves the camera to a **new address** after the reboot.

**Locked out?** If the saved Pi WiFi name or password is wrong, the camera falls back to its `espcam-<ID>-setup` AP after 15 s, and you can correct them there. If the admin or AP password is forgotten, the only way back is to re-flash the board over serial with the flash erased.

## 6. Stock camera endpoints

These come from the Espressif example and are mainly for aiming and focusing by hand. They keep the stock `GET` methods because the built-in `/ui` page calls them that way.

| Endpoint | Use |
|---|---|
| `GET /ui` | Full camera web UI (live preview, sensor settings) |
| `GET http://<ip>:81/stream` | MJPEG preview stream |
| `GET /capture`, `GET /bmp` | Take and return a fresh frame immediately (not the stored shot) |
| `GET /control?var=<name>&val=<n>` | Change a sensor setting |
| `GET /status` | Sensor register dump (not the same as `/shot_status`) |

- They require the camera to be **armed**, otherwise `503`.
- Open requests (a running stream) **block `POST /disarm`** and the idle timeout until they close.
- Settings changed through `/control` are lost on power-down, because the sensor is re-initialised on every arm. Treat them as temporary. Permanent defaults belong in `config.h`.
- The Pi's normal capture flow does not need any of these.

## 7. Recommended capture sequence

For a set of cameras `C = {0..7}`:

1. **Arm all.** `POST /arm` on every camera, one after another. Each returns at once (`202` or `200`). Then poll `GET /shot_status` on every camera until all show `"state":"armed"` (about 2-4 s). Retry or abort for any camera that ends up `disarmed` with an `error`, or is still `arming` after ~10 s.
2. **Lock exposure (once per session, or when lighting changes).** `POST /lock` on every camera while the scene is lit as it will be during the shot.
3. **Record the baseline.** `GET /shot_status` on every camera, keep each `shot_id` (call it `before[N]`) and `missed`. Optionally also verify `cam_id` and `mode == "sta"`.
4. **Fire.** Pulse the trigger GPIO high for ~5-10 ms, then low. Stay within the 30 s idle window after step 1 (unless auto power-down is off).
5. **Wait for the shots.** Poll `GET /shot_status` on each camera until `shot_id > before[N]`. Typical wait is a few hundred ms. Use a timeout of about 5 s. If it expires, see §8.
6. **Download.** `GET /shot` for each camera. Limit concurrency to about **2-4 cameras at once**; all 8 simultaneously on one 2.4 GHz AP works but is slower and may time out. Check `X-Shot-Id == before[N] + 1` and the JPEG validity checks from §5.4. Plan on roughly 10-20 s total for 8 images of about 1 MB each over WiFi (an estimate, not measured).
7. **Disarm all.** `POST /disarm` on every camera to cut heat. If you skip this, they power down by themselves 30 s after the last activity (never, if `idle_s` is 0), and `/shot` downloads extend that timer.

For repeated sequences without re-arming, repeat steps 3-6 as long as each camera stays armed. The cost is heat, which is the main reason this design powers cameras down.

## 8. Failure handling

| Symptom | Likely cause | Pi action |
|---|---|---|
| No reply at all from a camera | Not on the Pi's AP (fallback mode), powered off, or out of range | Retry; check power and AP signal. After a reboot a camera may take up to ~15 s to join |
| After `POST /arm`, `state` goes back to `disarmed` with an `error` | Sensor init failed | Retry once; check the camera ribbon cable and power |
| `shot_id` never increases | Trigger did not reach this camera (wiring, ground), or camera was not armed | Compare `missed` against the baseline. `missed` went up: the camera saw the pulse but was not armed or was busy. `missed` unchanged: the pulse never arrived (wiring) |
| `shot_id` increases but `/shot` has the wrong `X-Shot-Id` | A second trigger overwrote the shot | Re-shoot. Never trigger again before downloading |
| `uptime_ms` decreased, `state` is `disarmed`, `shot_id` is `0` | Camera rebooted (brown-out is the usual cause) | Re-arm and re-lock. Check the 5 V supply |
| `/shot` returns `404` | No shot since boot | As above |
| `error` field not empty | Last error from the camera (for example `frame larger than SHOT_BUFFER_SIZE`, `shot buffer busy, shot dropped`) | Read it; the shot may be missing |

## 9. Synchronisation notes

- The pulse reaches all cameras within microseconds. The residual difference between cameras comes from the **sensor**: it free-runs, so each camera starts its exposure at its next frame boundary. The offset can be up to about one frame period (likely 100-200 ms at QXGA on OV5640, to be measured). Strobe or very short bright lighting hides this.
- `trigger_us` and `latency_ms` use each camera's own clock and are **not comparable across cameras**. If the Pi needs a trigger time, record its own timestamp when it pulses the GPIO.
- Fixed exposure (`/lock`) matters more for the look of the final sequence than any other setting.

## 10. Quick reference

| Endpoint | Method | Needs armed | Purpose |
|---|---|---|---|
| `/shot_status` | GET | no | Status JSON (poll this) |
| `/arm` | POST | no | Start powering up the sensor (returns at once; poll for `armed`) |
| `/disarm` | POST | no | Power down sensor |
| `/trigger` | POST | yes | Software trigger (testing) |
| `/lock`, `/unlock` | POST | yes | Freeze / release exposure, gain, white balance |
| `/shot` | GET | no | Download the stored JPEG |
| `/update` | POST | no | OTA firmware update (`X-Auth`) |
| `/setup`, `/config` | GET | no | Settings page / current settings JSON (`/config.json` also works) |
| `/config` | PATCH | no | Change settings and reboot (`X-Auth`) |
| `/config` | DELETE | no | Factory reset and reboot (`X-Auth`) |
| `/` | GET | no | Browser test console |
| `/ui`, `/stream` (:81), `/capture`, `/control`, `/status` | GET | yes | Stock camera UI |
| GPIO13 | rising edge | yes | Hardware trigger (the real one) |

Quick check from a shell, without the Pi's GPIO:

```
curl -X POST http://192.168.4.10/arm
curl http://192.168.4.10/shot_status        # repeat until "state":"armed"
curl -X POST http://192.168.4.10/trigger
curl http://192.168.4.10/shot_status
curl -o test.jpg http://192.168.4.10/shot
curl -X POST http://192.168.4.10/disarm
```
