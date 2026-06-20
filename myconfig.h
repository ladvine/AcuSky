// NVS namespace and keys — shared between AcuSky.ino and app_httpd.cpp
#define NVS_NS           "acusky"
#define NVS_CAM_NAME     "cam_name"
#define NVS_SKIP_CAM     "skip_cam"
#define NVS_BOOT_COUNT   "boot_count"
#define NVS_LAST_REASONS "last_reasons"

#define CAM_NAME "Acusky"

/*
 * Sensor Support
 * Comment out to disable BME280 + AHT10 support.
 * Single define controls both AcuSky.ino and app_httpd.cpp.
 */
#define HAS_SENSORS

/*
 * camera name advertised on the network (mdns) for services and OTA
 */
#define MDNS_NAME "acusky-cam"

/*
 *    WiFi Settings
 *
 *  WiFi credentials are NO LONGER stored here.
 *  On first boot the device opens an AccessPoint with the name and password below.
 *  Connect to it and configure your WiFi network via the captive portal.
 *  Credentials are stored securely in ESP32 NVS flash and used on all subsequent boots.
 *
 *  To reconfigure WiFi: Web UI -> /dump page -> "Reset WiFi" button.
 */
#define WIFI_AP_NAME     "AcuSky-Setup"  // Portal AccessPoint name
#define WIFI_AP_PASSWORD "acusky123"     // Portal AccessPoint password

/* Extended Settings */

/*
 * If defined: URL_HOSTNAME will be used in place of the IP address in internal URL's
 */
#define URL_HOSTNAME "acusky-cam"

/*
 *  Port numbers for WebUI and Stream, defaults to 80 and 81.
 */
// #define HTTP_PORT 80
// #define STREAM_PORT 81

/*
 * Over The Air firmware updates; disable by uncommenting NO_OTA.
 * Password protect OTA to prevent unauthorised updates.
 * When enabled the device advertises itself using MDNS_NAME above.
 */
// #define NO_OTA
#define OTA_PASSWORD "change-me"

/* NTP
 *  Uncomment to enable the on-board clock.
 *  Pick a nearby pool server from: https://www.ntppool.org/zone/@
 *  Set the GMT offset to match your timezone IN SECONDS:
 *    see https://en.wikipedia.org/wiki/List_of_UTC_time_offsets
 *    1hr = 3600 seconds; do the math ;-)
 *    Default is CET (Central European Time), eg GMT + 1hr
 *  The DST offset is usually 1 hour (again, in seconds) if used in your country.
 */
#define NTPSERVER "0.in.pool.ntp.org"
#define NTP_GMT_OFFSET 19800
#define NTP_DST_OFFSET 0

/*
 * Camera Defaults
 *
 */
// Initial Reslolution, default SVGA
// available values are: FRAMESIZE_[THUMB|QQVGA|HQVGA|QVGA|CIF|HVGA|VGA|SVGA|XGA|HD|SXGA|UXGA] + [FHD|QXGA] for 3Mp Sensors; eg ov3660
// #define DEFAULT_RESOLUTION FRAMESIZE_SVGA

// Hardware Horizontal Mirror, 0 or 1 (overrides default board setting)
// #define H_MIRROR 0

// Hardware Vertical Flip , 0 or 1 (overrides default board setting)
// #define V_FLIP 1

// Browser Rotation (one of: -90,0,90, default 0)
// #define CAM_ROTATION 0

// Minimal frame duration in ms, used to limit max FPS
// max_fps = 1000/min_frame_time
// #define MIN_FRAME_TIME 500

/*
 * Additional Features
 *
 */
// Default Page: uncomment to make the full control page the default, otherwise show simple viewer
// #define DEFAULT_INDEX_FULL

// Uncomment to disable the notification LED on the module
// #define LED_DISABLE

// Uncomment to disable the illumination lamp features
// #define LAMP_DISABLE

// Define the startup lamp power setting (as a percentage, defaults to 0%)
// Saved (SPIFFS) user settings will override this
// #define LAMP_DEFAULT 0

// Assume the module used has a SPIFFS/LittleFS partition, and use that for persistent setting storage
// Uncomment to disable this this, the controls will still be shown in the UI but are inoperative.
// #define NO_FS

// Uncomment to enable camera debug info on serial by default
// #define DEBUG_DEFAULT_ON

/*
 * Camera Hardware Selectiom
 *
 * You must uncomment one, and only one, of the lines below to select your board model.
 * Remember to also select the board in the Boards Manager
 * This is not optional
 */
#define CAMERA_MODEL_AI_THINKER       // default
// #define CAMERA_MODEL_WROVER_KIT
// #define CAMERA_MODEL_ESP_EYE
// #define CAMERA_MODEL_M5STACK_PSRAM
// #define CAMERA_MODEL_M5STACK_V2_PSRAM
// #define CAMERA_MODEL_M5STACK_WIDE
// #define CAMERA_MODEL_M5STACK_ESP32CAM   // Originally: CAMERA_MODEL_M5STACK_NO_PSRAM
// #define CAMERA_MODEL_TTGO_T_JOURNAL
// #define CAMERA_MODEL_ARDUCAM_ESP32S_UNO

// Initial Camera module bus communications frequency
// Currently defaults to 8MHz
// The post-initialisation (runtime) value can be set and edited by the user in the UI
// For clone modules that have camera module and SPIFFS startup issues try setting
// this very low (start at 2MHZ and increase):
// #define XCLK_FREQ_MHZ 2
