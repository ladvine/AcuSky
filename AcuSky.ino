#include <esp_camera.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "time.h"
#include <ESPmDNS.h>
// --- AcuSky additions ---
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include "src/WiFiManager/WiFiManager.h"
#include <Preferences.h>

#if ESP_IDF_VERSION_MAJOR == 4
#include "esp_int_wdt.h"
#elif ESP_IDF_VERSION_MAJOR == 5
#include "esp_private/esp_int_wdt.h"
#include "esp_private/periph_ctrl.h"
#endif

/* This sketch is a extension/expansion/reork of the 'official' ESP32 Camera example
 *  sketch from Expressif:
 *  https://github.com/espressif/arduino-esp32/tree/master/libraries/ESP32/examples/Camera/CameraWebServer
 *
 *  It is modified to allow control of Illumination LED Lamps's (present on some modules),
 *  greater feedback via a status LED, and the HTML contents are present in plain text
 *  for easy modification.
 *
 *  A camera name can now be configured, and wifi details can be stored in an optional
 *  header file to allow easier updated of the repo.
 *
 *  The web UI has had changes to add the lamp control, rotation, a standalone viewer,
 *  more feeedback, new controls and other tweaks and changes,
 * note: Make sure that you have either selected ESP32 AI Thinker,
 *       or another board which has PSRAM enabled to use high resolution camera modes
 */


/*
 *  FOR NETWORK AND HARDWARE SETTINGS COPY OR RENAME 'myconfig.sample.h' TO 'myconfig.h' AND EDIT THAT.
 *
 * On first boot (or after a WiFi reset) the device opens an AccessPoint called
 * "AcuSky-Setup" (password: acusky123). Connect to it and configure your WiFi
 * network via the captive portal. Credentials are stored in NVS flash and used
 * on all subsequent boots automatically.
 *
 */

// Primary config, or defaults.
#if __has_include("myconfig.h")
    #include "myconfig.h"
#else
    #warning "Using Defaults: Copy myconfig.sample.h to myconfig.h and edit that to use your own settings"
    #define CAMERA_MODEL_AI_THINKER
#endif

// --- AcuSky: WiFiManager replaces stationList/WifiSetup ---
// Global WiFiManager instance (kept global so param_cam_name/param_xclk and
// the saveParamsCallback lambda remain valid for the object's lifetime)
WiFiManager wm;
// NVS for camera name persistence
Preferences devicePrefs;
// WiFiManager portal parameter (global so wm never holds a dangling pointer)
WiFiManagerParameter* param_cam_name = nullptr;
// BUG FIX: param_xclk must also be heap-allocated and global. It was previously
// a stack-local in WifiSetup() — wm.setSaveParamsCallback() captured it by
// reference, so if the portal ever reopened after WifiSetup() returned
// (e.g. via loop()'s wm.process()), the callback dereferenced freed stack memory.
WiFiManagerParameter* param_xclk = nullptr;

// Boot counter — incremented in stage0, exposed via /health endpoint
uint32_t bootCount = 0;

// Boot flags
bool skipCameraThisBoot = false;  // set in stage0, read in stage3

// Portal / sleep config
#define WIFI_PORTAL_TIMEOUT_SEC  180
#define WIFI_DEEP_SLEEP_SEC       30
// Camera retry config
#define MAX_CAMERA_RETRIES        3
#define CAMERA_RETRY_DELAY_MS  2000
#define CAMERA_POWER_SETTLE_MS  500
// NVS keys now defined in myconfig.h (NVS_NS, NVS_CAM_NAME, NVS_SKIP_CAM, NVS_BOOT_COUNT, NVS_LAST_REASONS)
// Subsystem availability flags (read by app_httpd.cpp)
bool cameraAvailable  = false;
bool sensorsAvailable = false;


/*
 *  use of BME280 Sensor on ESPCAM32, need https://github.com/finitespace/BME280 LIB
 *  use of AHT10 Sensor on ESPCAM32, need https://github.com/enjoyneering/AHT10/ LIB
 *   
 *     Connection diagram, can be change in main source code
 *     ESP32CAM    --  BME280, AHT10 Sensor
 *     GPIO 14     ->  SDA    
 *     GPIO 15     ->  SCL
 *     GND         ->  GND
 *     5V          ->  VIN     (3.3V was not working ???
 *
 *  #define HAS_SENSORS     in myconfig.h to include sensor support
*/ 

// Upstream version string
#include "src/version.h"

// Pin Mappings
#include "camera_pins.h"

// Camera config structure
camera_config_t config;

// Internal filesystem (SPIFFS)
// used for non-volatile camera settings
#include "storage.h"

// Sketch Info
int sketchSize;
int sketchSpace;
String sketchMD5;

// IP address, Netmask and Gateway, populated when connected
IPAddress ip;
IPAddress net;
IPAddress gw;

// Declare external function from app_httpd.cpp
extern void startCameraServer(int hPort, int sPort);
extern void serialDump();

// Names for the Camera. (initially from myconfig.h, can be overridden via WiFiManager portal)
#if defined(CAM_NAME)
    char myName[64] = CAM_NAME;
#else
    char myName[64] = "AcuSky";
#endif

#if defined(MDNS_NAME)
    char mdnsName[] = MDNS_NAME;
#else
    char mdnsName[] = "esp32-cam";
#endif

// Ports for http and stream (override in myconfig.h)
#if defined(HTTP_PORT)
    int httpPort = HTTP_PORT;
#else
    int httpPort = 80;
#endif

#if defined(STREAM_PORT)
    int streamPort = STREAM_PORT;
#else
    int streamPort = 81;
#endif

// Select between full and simple index as the default.
#if defined(DEFAULT_INDEX_FULL)
    char default_index[] = "full";
#else
    char default_index[] = "simple";
#endif

// Legacy stubs — app_httpd.cpp externs these; always false with WiFiManager
bool accesspoint   = false;
bool captivePortal = false;
char apName[64]    = WIFI_AP_NAME;  // reflects myconfig.h — stays in sync if user changes it

// The app and stream URLs
char httpURL[64] = {"Undefined"};
char streamURL[64] = {"Undefined"};

// Counters for info screens and debug
int8_t streamCount = 0;          // Number of currently active streams
unsigned long streamsServed = 0; // Total completed streams
unsigned long imagesServed = 0;  // Total image requests

// This will be displayed to identify the firmware
char myVer[] PROGMEM = __DATE__ " @ " __TIME__;

// This will be set to the sensors PID (identifier) during initialisation
int sensorPID;

// Camera module bus communications frequency.
// Originally: config.xclk_freq_mhz = 20000000, but this lead to visual artifacts on many modules.
// See https://github.com/espressif/esp32-camera/issues/150#issuecomment-726473652 et al.
#if !defined (XCLK_FREQ_MHZ)
    unsigned long xclk = 8;
#else
    unsigned long xclk = XCLK_FREQ_MHZ;
#endif

// initial rotation
// can be set in myconfig.h
#if !defined(CAM_ROTATION)
    #define CAM_ROTATION 0
#endif
int myRotation = CAM_ROTATION;

// minimal frame duration in ms, effectively 1/maxFPS
#if !defined(MIN_FRAME_TIME)
    #define MIN_FRAME_TIME 0
#endif
int minFrameTime = MIN_FRAME_TIME;

// Illumination LAMP and status LED
#if defined(LAMP_DISABLE)
    int lampVal = -1; // lamp is disabled in config
#elif defined(LAMP_PIN)
    #if defined(LAMP_DEFAULT)
        int lampVal = constrain(LAMP_DEFAULT,0,100); // initial lamp value, range 0-100
    #else
        int lampVal = 0; //default to off
    #endif
#else
    int lampVal = -1; // no lamp pin assigned
#endif

#if defined(LED_DISABLE)
    #undef LED_PIN    // undefining this disables the notification LED
#endif

bool autoLamp = false;         // Automatic lamp (auto on while camera running)

int lampChannel = 7;           // a free PWM channel (some channels used by camera)
const int pwmfreq = 50000;     // 50K pwm frequency
const int pwmresolution = 9;   // duty cycle bit range
const int pwmMax = pow(2,pwmresolution)-1;

#if defined(NO_FS)
    bool filesystem = false;
#else
    bool filesystem = true;
#endif

#if defined(NO_OTA)
    bool otaEnabled = false;
#else
    bool otaEnabled = true;
#endif

#if defined(OTA_PASSWORD)
    char otaPassword[] = OTA_PASSWORD;
#else
    char otaPassword[] = "";
#endif

#if defined(NTPSERVER)
    bool haveTime = true;
    const char* ntpServer = NTPSERVER;
    const long  gmtOffset_sec = NTP_GMT_OFFSET;
    const int   daylightOffset_sec = NTP_DST_OFFSET;
#else
    bool haveTime = false;
    const char* ntpServer = "";
    const long  gmtOffset_sec = 0;
    const int   daylightOffset_sec = 0;
#endif

// Critical error string; if set during init (camera hardware failure) it
// will be returned for all http requests
String critERR = "";


#if defined(HAS_SENSORS)
// (set these in myconfig.h)

  /*
  * BME280 Source code included from:
  * https://github.com/finitespace/BME280
  *
  */
  #include "src/bme280/BME280I2C.h"

  /*
  * AHT10 Source code included from:
  * https://github.com/enjoyneering/AHT10/
  *
  */
  #include "src/aht10/AHT10.h"
  #include <Wire.h>        

  #define I2C_SDA 14
  #define I2C_SCL 15

  BME280I2C::Settings settings(
   BME280::OSR_X1,
   BME280::OSR_X1,
   BME280::OSR_X1,
   BME280::Mode_Forced,
   BME280::StandbyTime_1000ms,
   BME280::Filter_Off,
   BME280::SpiEnable_False,
   BME280I2C::I2CAddr_0x76 // I2C address. I2C specific.
  );
  BME280I2C bme(settings);

  AHT10 esp32_aht10(AHT10_ADDRESS_0X38);
#endif

// Debug flag for stream and capture data
bool debugData;

void debugOn() {
    debugData = true;
    Serial.println("Camera debug data is enabled (send 'd' for status dump, or any other char to disable debug)");
}

void debugOff() {
    debugData = false;
    Serial.println("Camera debug data is disabled (send 'd' for status dump, or any other char to enable debug)");
}


// Serial input (debugging controls)
void handleSerial() {
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'd' ) {
            serialDump();
        } else {
            if (debugData) debugOff();
            else debugOn();
        }
    }
    while (Serial.available()) Serial.read();  // chomp the buffer
}

// Notification LED
void flashLED(int flashtime) {
#if defined(LED_PIN)                // If we have it; flash it.
    digitalWrite(LED_PIN, LED_ON);  // On at full power.
    delay(flashtime);              
     // delay
    digitalWrite(LED_PIN, LED_OFF); // turn Off
#else
    return;                         // No notifcation LED, do nothing, no delay
#endif
}

// Lamp Control
void setLamp(int newVal) {
#if defined(LAMP_PIN)
    if (newVal != -1) {
        // Apply a logarithmic function to the scale.
        int brightness = round((pow(2,(1+(newVal*0.02)))-2)/6*pwmMax);
        ledcWrite(lampChannel, brightness);
        Serial.print("Lamp: ");
        Serial.print(newVal);
        Serial.print("%, pwm = ");
        Serial.println(brightness);
    }
#endif
}

void printLocalTime(bool extraData=false) {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("Failed to obtain time");
    } else {
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S, %A, %B %d %Y", &timeinfo);
        Serial.println(timeBuf);
    }
    if (extraData) {
        Serial.printf("NTP Server: %s, GMT Offset: %li(s), DST Offset: %i(s)\r\n", ntpServer, gmtOffset_sec, daylightOffset_sec);
    }
}

void calcURLs() {
    // Set the URL's
    #if defined(URL_HOSTNAME)
        if (httpPort != 80) {
            sprintf(httpURL, "http://%s:%d/", URL_HOSTNAME, httpPort);
        } else {
            sprintf(httpURL, "http://%s/", URL_HOSTNAME);
        }
        sprintf(streamURL, "http://%s:%d/", URL_HOSTNAME, streamPort);
    #else
        if (httpPort != 80) {
            sprintf(httpURL, "http://%d.%d.%d.%d:%d/", ip[0], ip[1], ip[2], ip[3], httpPort);
        } else {
            sprintf(httpURL, "http://%d.%d.%d.%d/", ip[0], ip[1], ip[2], ip[3]);
        }
        sprintf(streamURL, "http://%d.%d.%d.%d:%d/", ip[0], ip[1], ip[2], ip[3], streamPort);
    #endif
}

void StartCamera() {
    // Populate camera config structure with hardware and other defaults
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
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = xclk * 1000000;
    config.pixel_format = PIXFORMAT_JPEG;
    // Pre-allocate large buffers
    if(psramFound()){
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    #if defined(CAMERA_MODEL_ESP_EYE)
        pinMode(13, INPUT_PULLUP);
        pinMode(14, INPUT_PULLUP);
    #endif

    // Reduce WiFi TX power + disable brownout detector for the camera init inrush spike.
    // Camera + WiFi transmit simultaneously is the worst-case current draw scenario.
    // CAMERA_POWER_SETTLE_MS gives the PSU time to stabilise before the spike.
    // Brownout detector is disabled for the narrowest possible window — re-enabled immediately.
    WiFi.setTxPower(WIFI_POWER_7dBm);
    delay(CAMERA_POWER_SETTLE_MS);
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable during inrush only

    esp_err_t err = esp_camera_init(&config);

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);  // ALWAYS re-enable
    WiFi.setTxPower(WIFI_POWER_19_5dBm);         // ALWAYS restore TX power

    if (err != ESP_OK) {
        delay(100);  // need a delay here or the next serial o/p gets missed
        Serial.printf("\r\n\r\nCRITICAL FAILURE: Camera sensor failed to initialise.\r\n\r\n");
        Serial.printf("A full (hard, power off/on) reboot will probably be needed to recover from this.\r\n");
        Serial.printf("Meanwhile; WiFi, OTA and sensors remain active.\r\n");
        // Reset the I2C bus.. may help when rebooting.
        periph_module_disable(PERIPH_I2C0_MODULE); // try to shut I2C down properly in case that is the problem
        periph_module_disable(PERIPH_I2C1_MODULE);
        periph_module_reset(PERIPH_I2C0_MODULE);
        periph_module_reset(PERIPH_I2C1_MODULE);
        // Camera failure is non-fatal — WiFi, OTA and sensors remain active.
        // The 60s WDT reboot has been removed: it would defeat the resilient boot
        // design and prevent OTA recovery when camera hardware is faulty.
        if (critERR.length() == 0) {
            critERR = "<h1>Error!</h1><hr><p>Camera module failed to initialise!</p>";
            critERR += "<p>Check power supply (5V/1A+) and ribbon cable.</p>";
            critERR += "<p>WiFi, OTA updates and sensors remain active.</p>";
        }
    } else {
        Serial.println("Camera init succeeded");
        cameraAvailable = true;  // BUG FIX: was never set true — all camera/stream
                                  // endpoints permanently returned 503 even on success

        // Get a reference to the sensor
        sensor_t * s = esp_camera_sensor_get();

        // Dump camera module, warn for unsupported modules.
        sensorPID = s->id.PID;
        switch (sensorPID) {
            case OV9650_PID: Serial.println("WARNING: OV9650 camera module is not properly supported, will fallback to OV2640 operation"); break;
            case OV7725_PID: Serial.println("WARNING: OV7725 camera module is not properly supported, will fallback to OV2640 operation"); break;
            case OV2640_PID: Serial.println("OV2640 camera module detected"); break;
            case OV3660_PID: Serial.println("OV3660 camera module detected"); break;
            default: Serial.println("WARNING: Camera module is unknown and not properly supported, will fallback to OV2640 operation");
        }

        // OV3660 initial sensors are flipped vertically and colors are a bit saturated
        if (sensorPID == OV3660_PID) {
            s->set_vflip(s, 1);  //flip it back
            s->set_brightness(s, 1);  //up the blightness just a bit
            s->set_saturation(s, -2);  //lower the saturation
        }

        // M5 Stack Wide has special needs
        #if defined(CAMERA_MODEL_M5STACK_WIDE)
            s->set_vflip(s, 1);
            s->set_hmirror(s, 1);
        #endif

        // Config can override mirror and flip
        #if defined(H_MIRROR)
            s->set_hmirror(s, H_MIRROR);
        #endif
        #if defined(V_FLIP)
            s->set_vflip(s, V_FLIP);
        #endif

        // set initial frame rate
        #if defined(DEFAULT_RESOLUTION)
            s->set_framesize(s, DEFAULT_RESOLUTION);
        #else
            s->set_framesize(s, FRAMESIZE_SVGA);
        #endif

        /*
        * To override default camera settings at startup, uncomment lines here:
        * https://github.com/espressif/esp32-camera/blob/master/driver/include/sensor.h#L149
        */
    }  // end else (camera init succeeded)
    // We now have camera with default init
}

// =============================================================================
// NVS helpers
// =============================================================================
void loadDevicePrefs() {
    devicePrefs.begin(NVS_NS, true);
    String n = devicePrefs.getString(NVS_CAM_NAME, "");
    if (n.length() > 0) n.toCharArray(myName, sizeof(myName));
    // E7: load saved XCLK (if set via portal)
    uint32_t savedXclk = devicePrefs.getUInt("xclk", 0);
    if (savedXclk >= 2 && savedXclk <= 20) xclk = savedXclk;
    devicePrefs.end();
}

void saveDevicePrefs(const char* name) {
    devicePrefs.begin(NVS_NS, false);
    devicePrefs.putString(NVS_CAM_NAME, name);
    devicePrefs.end();
}

void setSkipCameraFlag(bool skip) {
    devicePrefs.begin(NVS_NS, false);
    devicePrefs.putBool(NVS_SKIP_CAM, skip);
    devicePrefs.end();
}

// =============================================================================
// STAGE 0 — Power settle + brownout detection
// =============================================================================
void stage0_powerSettle() {
    delay(300);  // let PSU fully stabilise before touching anything
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("Reset reason: %d  ", reason);
    bool isBrownout = (reason == ESP_RST_BROWNOUT);
    const char* reasonStr = "unknown";
    switch(reason) {
        case ESP_RST_POWERON:   Serial.println("(cold boot)");       reasonStr = "poweron";   break;
        case ESP_RST_SW:        Serial.println("(software reset)");  reasonStr = "sw_reset";  break;
        case ESP_RST_DEEPSLEEP: Serial.println("(deep sleep wake)"); reasonStr = "deepsleep"; break;
        case ESP_RST_BROWNOUT:  Serial.println("(BROWNOUT)");        reasonStr = "brownout";  break;
        case ESP_RST_WDT:       Serial.println("(watchdog)");        reasonStr = "watchdog";  break;
        default:                Serial.printf("(code %d)\n", reason); reasonStr = "other";    break;
    }

    // E1: Update boot counter and rolling reset reason history in NVS
    devicePrefs.begin(NVS_NS, false);
    uint32_t bootCount = devicePrefs.getUInt(NVS_BOOT_COUNT, 0) + 1;
    devicePrefs.putUInt(NVS_BOOT_COUNT, bootCount);
    ::bootCount = bootCount;  // update the global for /health endpoint
    // Keep last 5 reset reasons as comma-separated string
    String history = devicePrefs.getString(NVS_LAST_REASONS, "");
    if (history.length() > 0) history += ",";
    history += String(reasonStr);
    // Trim to last 5 entries
    int commaCount = 0;
    for (int i = history.length() - 1; i >= 0; i--) {
        if (history[i] == ',') commaCount++;
        if (commaCount >= 5) { history = history.substring(i + 1); break; }
    }
    devicePrefs.putString(NVS_LAST_REASONS, history);
    devicePrefs.end();

    Serial.printf("Boot count: %u\n", bootCount);

    // FIX: simplified to a true one-shot. The previous logic persisted a
    // "skip again next boot" NVS flag across the reset boundary, which could
    // get permanently re-armed if any unrelated reset (SW reset during
    // flashing, watchdog, etc.) happened to land while the flag was set —
    // resulting in the camera being skipped on every single boot forever.
    // Now: skip ONLY on the boot where brownout was the actual reset reason.
    // Every other boot — including the very next one — attempts the camera.
    if (isBrownout) {
        Serial.println(">> BROWNOUT: camera skipped this boot only.");
        skipCameraThisBoot = true;
        critERR  = "<h1>PSU Recovery Boot</h1><hr>";
        critERR += "<p>Last reset was a brownout. Camera skipped to protect PSU.</p>";
        critERR += "<p>WiFi, OTA and sensors remain active.</p>";
    } else {
        Serial.println(">> Normal boot.");
        skipCameraThisBoot = false;
    }
}

// =============================================================================
// STAGE 1 — WiFi via WiFiManager
// =============================================================================
void WifiSetup() {
    Serial.println("Starting WiFi (WiFiManager)");
    flashLED(300); delay(100); flashLED(300);

    loadDevicePrefs();

    // Allocate portal parameters on heap so wm never holds dangling pointers
    if (param_cam_name) delete param_cam_name;
    param_cam_name = new WiFiManagerParameter(NVS_CAM_NAME, "Camera Name", myName, 63);

    // E7: XCLK frequency as portal parameter — key tuning knob for brownout issues
    char xclkStr[4];
    snprintf(xclkStr, sizeof(xclkStr), "%lu", xclk);
    // BUG FIX: heap-allocated like param_cam_name — was a stack local, causing
    // a dangling reference if the portal reopened after WifiSetup() returned.
    if (param_xclk) delete param_xclk;
    param_xclk = new WiFiManagerParameter("xclk", "XCLK MHz (2=low-power, 8=default, 20=fast)", xclkStr, 3);

    wm.addParameter(param_cam_name);
    wm.addParameter(param_xclk);
    wm.setDebugOutput(false);
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);
    // Without this, autoConnect() can hang indefinitely while attempting to
    // connect with SAVED credentials if the router is slow/unreachable —
    // separate from, and before, the config-portal-timeout logic below ever
    // gets a chance to run. This is the library author's documented fix for
    // exactly this class of hang: "If trying to connect ends up in an
    // endless loop, try setConnectTimeout(60) before autoConnect()."
    wm.setConnectTimeout(30);
    WiFi.setSleep(false);
    // NOTE: WiFi.setTxPower() removed from here — it was being called before
    // wm.autoConnect() initializes the WiFi radio/driver (WiFi.mode(), etc).
    // Calling setTxPower() on an uninitialized radio can hang the driver.
    // TX power is now only adjusted around camera init (StartCamera()),
    // after WiFi is already confirmed connected.
    //
    // NOTE: setConfigPortalBlocking(false) + manual wm.process() polling was
    // removed. That combination is a known source of unreliable/hanging AP
    // startup in WiFiManager (the portal sometimes doesn't come up cleanly
    // in non-blocking mode). autoConnect() in its default BLOCKING mode
    // already does exactly what headless provisioning needs: it tries the
    // saved network first, and if that fails it automatically starts the
    // "AcuSky-Setup" AP + captive portal and waits there until the user
    // configures WiFi or the portal timeout below is hit.

    wm.setAPCallback([](WiFiManager* w) {
        Serial.printf("Config portal open: SSID='AcuSky-Setup' IP=%s\n",
                      WiFi.softAPIP().toString().c_str());
        for (int i = 0; i < 8; i++) { flashLED(80); delay(80); }
    });

    wm.setSaveParamsCallback([&]() {
        const char* n = param_cam_name->getValue();
        if (strlen(n) > 0) {
            strncpy(myName, n, sizeof(myName) - 1);
            myName[sizeof(myName) - 1] = '\0';
            saveDevicePrefs(myName);
        }
        // E7: Save XCLK from portal
        int newXclk = atoi(param_xclk->getValue());
        if (newXclk >= 2 && newXclk <= 20) {
            xclk = newXclk;
            devicePrefs.begin(NVS_NS, false);
            devicePrefs.putUInt("xclk", xclk);
            devicePrefs.end();
            Serial.printf("XCLK saved: %lu MHz\n", xclk);
        }
    });

    // Blocking call: returns true if connected via saved credentials, or
    // after the user successfully configures WiFi through the portal.
    // Returns false only if the portal timed out with no configuration.
    bool connected = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASSWORD);

    if (!connected) {
        Serial.printf("Portal timed out after %ds. Sleeping %ds then retrying.\n",
                      WIFI_PORTAL_TIMEOUT_SEC, WIFI_DEEP_SLEEP_SEC);
        flashLED(2000);
        esp_sleep_enable_timer_wakeup((uint64_t)WIFI_DEEP_SLEEP_SEC * 1000000ULL);
        esp_deep_sleep_start();
    }

    // WiFi confirmed connected — explicitly set full TX power for best signal/throughput
    delay(200);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    accesspoint = false;
    ip  = WiFi.localIP();
    net = WiFi.subnetMask();
    gw  = WiFi.gatewayIP();
    Serial.printf("WiFi connected: %d.%d.%d.%d  RSSI: %d dBm\n",
                  ip[0],ip[1],ip[2],ip[3], WiFi.RSSI());
    calcURLs();
    for (int i = 0; i < 5; i++) { flashLED(50); delay(150); }
}

#if defined(HAS_SENSORS)

  float getBME280_hum() { 
      if (esp32_aht10.readRawData() != AHT10_ERROR)
      {
        return esp32_aht10.readTemperature(AHT10_USE_READ_DATA);
      }
      return 0;
      }

  float getBME280_temp() {
      BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);   // you can change Unit to TempUnit_Fahrenheit 
      return bme.temp(tempUnit);
      }

  float getBME280_pres(){ 
      BME280::PresUnit presUnit(BME280::PresUnit_hPa);      // you can change Unit here https://github.com/finitespace/BME280#tempunit-enum
      return bme.pres(presUnit);
  } 

#else

  float getBME280_hum()  { return 0; }
  float getBME280_temp() { return 0; }
  float getBME280_pres() { return 0; }

#endif

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();
    Serial.println("====");
    Serial.print("AcuSky Server: "); Serial.println(myName);
    Serial.print("Code Built: ");    Serial.println(myVer);
    Serial.print("Base Release: ");  Serial.println(baseVersion);
    Serial.println();

    #if defined(LED_PIN)
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, LED_ON);
    #endif

    // STAGE 0: Power settle + brownout detection (sets skipCameraThisBoot)
    stage0_powerSettle();

    // Start filesystem early (needed by loadPrefs after camera comes up)
    if (filesystem) {
        filesystemStart();
        delay(200);
    }

    // STAGE 1: WiFi — must succeed before anything else; enables OTA recovery
    WifiSetup();

    // STAGE 2: OTA + mDNS + NTP — set up immediately after WiFi
    if (otaEnabled) {
        Serial.println("Setting up OTA");
        ArduinoOTA.setHostname(mdnsName);
        if (strlen(otaPassword) != 0) {
            ArduinoOTA.setPassword(otaPassword);
            Serial.println("OTA Password: [set]");
        } else {
            Serial.printf("\r\nNo OTA password has been set! (insecure)\r\n\r\n");
        }
        ArduinoOTA
            .onStart([]() {
                String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
                Serial.println("Start updating " + type);
                if (cameraAvailable) {
                    esp_camera_deinit();
                    cameraAvailable = false;
                }
                critERR = "<h1>OTA Has been started</h1><hr><p>Camera has Halted!</p>";
                critERR += "<p>Wait for OTA to finish and reboot, or <a href=\"control?var=reboot&val=0\">reboot manually</a></p>";
            })
            .onEnd([]() { Serial.println("\r\nEnd"); })
            .onProgress([](unsigned int progress, unsigned int total) {
                if (total > 0) Serial.printf("Progress: %u%%\r", (progress * 100) / total);
            })
            .onError([](ota_error_t error) {
                Serial.printf("Error[%u]: ", error);
                if (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
                else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
                else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
                else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
                else if (error == OTA_END_ERROR)     Serial.println("End Failed");
            });
        ArduinoOTA.begin();
    } else {
        Serial.println("OTA is disabled");
        if (!MDNS.begin(mdnsName)) Serial.println("Error setting up MDNS responder!");
        else Serial.println("mDNS responder started");
    }
    // Fix 4: addService is safe in both branches above — ArduinoOTA.begin() calls
    // MDNS.begin() internally when OTA is enabled, and the explicit else branch
    // above logs (but doesn't block on) failure. ESPmDNS tolerates addService()
    // being called even if begin() failed; it simply has no effect.
    MDNS.addService("http", "tcp", httpPort);
    MDNS.addService("http", "tcp", streamPort);  // E3: advertise stream port so tools auto-discover it

    if (haveTime) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        Serial.printf("NTP: syncing with %s...\n", ntpServer);
        // Wait up to 5s for SNTP sync — configTime() is async
        struct tm t;
        unsigned long ntpWait = millis();
        while (!getLocalTime(&t, 0) && millis() - ntpWait < 5000) delay(200);
        printLocalTime(true);
    } else {
        Serial.println("Time functions disabled");
    }

    // STAGE 3: Camera — non-fatal, brownout-aware, retried
    if (!psramFound()) {
        Serial.println("WARNING: No PSRAM found, max resolution SVGA");
        // Note: no longer halting here — continue without PSRAM
    }
    if (skipCameraThisBoot) {
        Serial.println("Camera SKIPPED (brownout recovery boot).");
    } else {
        for (int attempt = 1; attempt <= MAX_CAMERA_RETRIES && !cameraAvailable; attempt++) {
            Serial.printf("Camera init: attempt %d/%d\n", attempt, MAX_CAMERA_RETRIES);
            StartCamera();
            if (!cameraAvailable && attempt < MAX_CAMERA_RETRIES) {
                esp_task_wdt_reset();  // prevent WDT reset during retry loop
                delay(CAMERA_RETRY_DELAY_MS);
            }
        }
    }

    if (cameraAvailable && filesystem) {
        delay(200);
        loadPrefs(SPIFFS);
    } else if (!filesystem) {
        Serial.println("No Internal Filesystem, cannot load or save preferences");
    }

    // Lamp init
    if (lampVal != -1) {
        #if defined(LAMP_PIN)
            #if ESP_IDF_VERSION_MAJOR == 4
                ledcSetup(lampChannel, pwmfreq, pwmresolution);
                ledcAttachPin(LAMP_PIN, lampChannel);
            #elif ESP_IDF_VERSION_MAJOR == 5
                ledcAttachChannel(LAMP_PIN, pwmfreq, pwmresolution, lampChannel);
            #endif
            if (autoLamp) setLamp(0);
            else setLamp(lampVal);
        #endif
    } else {
        Serial.println("No lamp, or lamp disabled in config");
    }

    // STAGE 4: Sensors — non-fatal
    #if defined(HAS_SENSORS)
    Wire.begin(I2C_SDA, I2C_SCL);
    if (!bme.begin()) {
        Serial.println("BME280 not found — sensors unavailable");
        sensorsAvailable = false;
    } else {
        switch(bme.chipModel()) {
            case BME280::ChipModel_BME280: Serial.println("Found BME280 sensor!"); break;
            case BME280::ChipModel_BMP280: Serial.println("Found BMP280 sensor! No Humidity available."); break;
            default: Serial.println("Found UNKNOWN sensor!");
        }
        settings.tempOSR = BME280::OSR_X4;
        bme.setSettings(settings);
        delay(AHT10_POWER_ON_DELAY);
        Wire.setClock(100000);
        esp32_aht10.softReset();
        Serial.println(F("AHT10 OK"));
        sensorsAvailable = true;
    }
    #endif

    // STAGE 5: HTTP server — always starts regardless of subsystem state
    sketchSize  = ESP.getSketchSize();
    sketchSpace = ESP.getFreeSketchSpace();
    sketchMD5   = ESP.getSketchMD5();

    startCameraServer(httpPort, streamPort);

    Serial.println("\r\n====");
    Serial.printf("WiFi:    %d.%d.%d.%d\r\n", ip[0],ip[1],ip[2],ip[3]);
    Serial.printf("Camera:  %s\r\n", cameraAvailable  ? "OK" : "UNAVAILABLE");
    Serial.printf("Sensors: %s\r\n", sensorsAvailable ? "OK" : "UNAVAILABLE");
    if (critERR.length() == 0) {
        Serial.printf("URL:     %s\r\n", httpURL);
        Serial.printf("Stream:  %sview\r\n", streamURL);
    }
    Serial.println("====");

    #if defined(DEBUG_DEFAULT_ON)
        debugOn();
    #else
        debugOff();
    #endif

    while (Serial.available()) Serial.read();
}


void loop() {
    if (otaEnabled) ArduinoOTA.handle();

    handleSerial();

    // WiFi status monitoring — checked every 5s.
    // NOTE: ESP32's built-in WiFi auto-reconnect is NOT reliable for all
    // disconnect reasons (see espressif/arduino-esp32 issue #7210 and
    // multiple community reports of it silently failing to reconnect).
    // We explicitly call WiFi.reconnect() here rather than relying on it.
    static unsigned long lastWifiCheck = 0;
    static bool wifiWasUp = true;
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        bool wifiNow = (WiFi.status() == WL_CONNECTED);
        if (wifiWasUp && !wifiNow) {
            Serial.println("WiFi disconnected, attempting reconnect...");
            WiFi.reconnect();
            wifiWasUp = false;
        } else if (!wifiWasUp && wifiNow) {
            ip = WiFi.localIP(); calcURLs();
            Serial.printf("WiFi reconnected: %d.%d.%d.%d\n", ip[0],ip[1],ip[2],ip[3]);
            wifiWasUp = true;
        } else if (!wifiWasUp && !wifiNow) {
            // Still disconnected after 5s — retry
            Serial.println("WiFi still disconnected, retrying...");
            WiFi.reconnect();
        }
    }

    // E2: Periodic camera retry — if camera failed at boot, retry once per hour.
    // Covers transient failures: marginal PSU that recovered, loose ribbon cable reseated.
    // Guarded against streamCount and OTA: StartCamera() throttles WiFi TX power and
    // disables the brownout detector briefly, which would disrupt an active stream
    // or an in-progress OTA update if it ran concurrently.
    static unsigned long lastCamRetry = 0;
    bool otaInProgress = (critERR.indexOf("OTA") >= 0);
    if (!cameraAvailable && streamCount == 0 && !otaInProgress &&
        (millis() - lastCamRetry > 3600000UL)) {
        lastCamRetry = millis();
        Serial.println("Periodic camera retry...");
        StartCamera();
        if (cameraAvailable) {
            Serial.println("Camera recovered! Loading prefs.");
            if (filesystem) loadPrefs(SPIFFS);
            critERR = "";  // clear error so UI shows camera controls again
        }
    }

    delay(10);
}
