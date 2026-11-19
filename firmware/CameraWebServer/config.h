#ifndef CONFIG_H
#define CONFIG_H

// =====================================================================
// FACTORY DEFAULTS ONLY.
// Every board gets the same firmware. On first boot it has no saved settings, so it starts a
// setup AP ("espcam-setup-XXXX", password FALLBACK_AP_PASSWORD, page at http://192.168.4.1/setup).
// Anything saved there (ID, WiFi, IP scheme, passwords, resolution, power settings...) is stored
// in flash and overrides the values below. The values below are used for fields that were never
// saved and after "Reset to factory defaults".
// =====================================================================
// Default camera ID (0-7). Set the real one from the /setup page.
// =====================================================================
#ifndef CAM_ID
#define CAM_ID 0
#endif
static_assert(CAM_ID >= 0 && CAM_ID <= 7, "CAM_ID must be 0..7");

// =====================================================================
// Network: the Raspberry Pi's access point (static addressing, no DHCP)
// Camera N gets  NET_PREFIX . (CAM_IP_HOST_BASE + N)   e.g. 192.168.4.10 for cam 0 ... 192.168.4.17 for cam 7
// Keep the range outside the Pi's DHCP pool. The Pi AP must be 2.4 GHz.
// =====================================================================
#define WIFI_SSID "timeslice"
#define WIFI_PASSWORD "change-me-pi-ap"

#define NET_PREFIX_BYTES 192, 168, 4
#define NET_GATEWAY_BYTES 192, 168, 4, 1  // the Raspberry Pi
#define NET_SUBNET_BYTES 255, 255, 255, 0
#define CAM_IP_HOST_BASE 10

#define STA_TIMEOUT_MS 15000         // how long to try the Pi's AP before falling back
#define STA_RETRY_INTERVAL_MS 60000  // while in fallback AP with no clients, retry the Pi's AP this often

// Reduce radio heat: the cameras sit close to the Pi. Options: WIFI_POWER_2dBm ... WIFI_POWER_19_5dBm
#define WIFI_TX_POWER WIFI_POWER_11dBm

// =====================================================================
// Fallback access point (used when the Pi's AP cannot be found)
// SSID is "espcam-<CAM_ID>-setup". Password must be at least 8 characters.
// Web test console is at http://192.168.4.1/
// =====================================================================
#define FALLBACK_AP_PASSWORD "espcam-setup"
#define FALLBACK_AP_IP_BYTES 192, 168, 4, 1
#define FALLBACK_AP_CHANNEL 1

// =====================================================================
// OTA firmware update: POST the .bin to /update with header "X-Auth: <OTA_PASSWORD>"
// =====================================================================
#define OTA_PASSWORD "change-me-ota"

// =====================================================================
// Trigger input (rising edge from the Raspberry Pi, 3.3 V logic)
// GPIO13 is free on the AI-Thinker board as long as the SD slot is unused.
// Avoid 0, 2, 12, 15 (strapping pins).
// =====================================================================
#define TRIGGER_GPIO_NUM 13
#define TRIGGER_DEBOUNCE_MS 50

// =====================================================================
// Camera / shot settings
// =====================================================================
#define SHOT_FRAMESIZE FRAMESIZE_QXGA  // 2048x1536. FRAMESIZE_QSXGA = 2560x1920 (slow, big files; raise SHOT_BUFFER_SIZE)
#define SHOT_JPEG_QUALITY 10           // 0-63, lower = better quality / bigger file
#define SHOT_BUFFER_SIZE (1536 * 1024)  // PSRAM reserved for the last shot; raise for 5MP
#define SHOT_VFLIP 0
#define SHOT_HMIRROR 0
#define CAMERA_XCLK_HZ 20000000  // 10000000 runs cooler but lowers frame rate (more trigger jitter)

// Frames are discarded for this long after power-up so auto exposure / white balance settle.
// Skipped (short flush only) when exposure/white balance have been locked with /lock (OV5640).
#define ARM_WARMUP_MS 1500

// =====================================================================
// Power / heat
// =====================================================================
#define CPU_FREQ_MHZ 160     // 80, 160 or 240
#define IDLE_DISARM_MS 30000  // camera powers itself down this long after the last activity (0 = never)

#endif  // CONFIG_H
