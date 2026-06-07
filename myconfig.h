/*
 * AcuSky - myconfig.h
 * ============================================================
 * WiFi credentials are NO LONGER stored here.
 * They are configured at first boot via the "AcuSky-Setup"
 * captive portal and stored securely in ESP32 NVS flash.
 *
 * To reconfigure WiFi: Web UI → Control → "Reset WiFi & Reboot"
 * Camera name is also set via the portal and persisted to NVS.
 * CAM_NAME below is the factory default used only on very first boot.
 */

// ── Identity ──────────────────────────────────────────────────────────────────

// Factory default camera name (overridden by portal after first setup)
#define CAM_NAME "AcuSky"

// mDNS hostname — used for OTA and local DNS: http://acusky-cam.local/
#define MDNS_NAME "acusky-cam"

// Use hostname in URLs instead of IP address
#define URL_HOSTNAME "acusky-cam"

// ── Ports ─────────────────────────────────────────────────────────────────────

// #define HTTP_PORT    80
// #define STREAM_PORT  81

// ── OTA Updates ───────────────────────────────────────────────────────────────

// OTA is critical for headless recovery — do not disable unless necessary
// #define NO_OTA

// Strongly recommended: set a password
#define OTA_PASSWORD "acusky123"

// ── NTP Time ──────────────────────────────────────────────────────────────────

// Pick the closest pool: https://www.ntppool.org/zone/@
// IST = UTC+5:30 = 19800 seconds
#define NTPSERVER      "0.in.pool.ntp.org"
#define NTP_GMT_OFFSET  19800
#define NTP_DST_OFFSET  0

// ── Camera Board ──────────────────────────────────────────────────────────────

// Uncomment exactly ONE board
#define CAMERA_MODEL_AI_THINKER
// #define CAMERA_MODEL_WROVER_KIT
// #define CAMERA_MODEL_ESP_EYE
// #define CAMERA_MODEL_M5STACK_PSRAM
// #define CAMERA_MODEL_M5STACK_V2_PSRAM
// #define CAMERA_MODEL_M5STACK_WIDE
// #define CAMERA_MODEL_M5STACK_ESP32CAM
// #define CAMERA_MODEL_TTGO_T_JOURNAL
// #define CAMERA_MODEL_ARDUCAM_ESP32S_UNO

// ── Camera Clock (XCLK) ───────────────────────────────────────────────────────
//
// BROWNOUT TIP: Lower XCLK = lower inrush current spike at camera init.
// Start at 8 MHz. If you see brownouts during camera init even after all
// software fixes, drop this to 4 or even 2 MHz. Increase only if your
// power supply is confirmed stable at 5V/1A+.
//
// Recommended values:
//   2 MHz — very marginal PSU (weak USB port, long cable)
//   4 MHz — borderline PSU
//   8 MHz — default, works well with most decent supplies
//  20 MHz — good dedicated 5V/1A PSU with bulk capacitor
#define XCLK_FREQ_MHZ 8

// ── Camera Defaults ───────────────────────────────────────────────────────────

// Starting resolution. Options:
// FRAMESIZE_QQVGA / QVGA / VGA / SVGA / XGA / HD / SXGA / UXGA
// #define DEFAULT_RESOLUTION FRAMESIZE_SVGA

// Hardware mirror / flip overrides
// #define H_MIRROR 0
// #define V_FLIP   1

// Browser-side rotation (-90, 0, or 90 degrees)
// #define CAM_ROTATION 0

// Minimum ms between frames (limits max FPS). 0 = unlimited
// #define MIN_FRAME_TIME 0

// ── LED / Lamp ────────────────────────────────────────────────────────────────

// #define LED_DISABLE
// #define LAMP_DISABLE
// #define LAMP_DEFAULT 0   // 0-100 percent

// ── Filesystem (SPIFFS) ───────────────────────────────────────────────────────
// Used to persist camera image settings across reboots.
// #define NO_FS

// ── Debug ─────────────────────────────────────────────────────────────────────
// #define DEBUG_DEFAULT_ON
