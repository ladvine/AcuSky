//#include <WiFiManager.h>
/*#include <strings_en.h>
#include <wm_consts_de.h>
#include <wm_consts_en.h>
#include <wm_consts_fr.h>
#include <wm_strings_de.h>
#include <wm_strings_en.h>
#include <wm_strings_es.h>
#include <wm_strings_fr.h>
#include <wm_strings_pt.h>
#include <wm_strings_pt_br.h>*/

/*
 * AcuSky - Live Weather Camera
 * ============================================================
 * BOOT PHILOSOPHY: Resilient, fault-tolerant, brownout-aware
 *
 * Priority order — each stage is independent. No failure stops the next stage.
 *
 *   STAGE 0 — Power settle + brownout detection
 *             Detect if we just brownout-reset. If so, skip the camera
 *             on this boot cycle to let the PSU recover. WiFi only.
 *
 *   STAGE 1 — WiFi (via WiFiManager)
 *             MUST succeed before anything else. Enables OTA recovery.
 *             Portal timeout → deep sleep → retry. Never hangs.
 *
 *   STAGE 2 — OTA + mDNS + NTP
 *             Immediately after WiFi. Broken hardware can still be updated.
 *
 *   STAGE 3 — Camera (non-fatal)
 *             Skipped if last reset was brownout (PSU too weak for cam spike).
 *             Retried with delay on transient failures.
 *             cameraAvailable flag gates all cam endpoints.
 *
 *   STAGE 4 — Sensors (non-fatal)
 *             sensorsAvailable flag gates all sensor endpoints.
 *
 *   STAGE 5 — HTTP server (always starts)
 *             Serves whatever subsystems came up.
 *
 * KEY BROWNOUT STRATEGIES:
 *   - Brownout detector disabled ONLY during camera init (peak inrush)
 *     and re-enabled immediately after — never disabled globally.
 *   - WiFi TX power reduced during camera init to prevent concurrent spikes.
 *   - Power settle delay before each high-current subsystem.
 *   - Reset reason checked: brownout → skip camera for one boot cycle.
 *     NVS flag remembers this across the reset boundary.
 *   - Camera init retried up to MAX_CAMERA_RETRIES with delay between.
 *   - XCLK frequency configurable low (2-8MHz) for marginal PSU boards.
 */

// ── Brownout + SOC control — must be first ────────────────────────────────────
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "esp_task_wdt.h"

#include <WiFi.h>
#include "src/WiFiManager-master/WiFiManager.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "time.h"

#if ESP_IDF_VERSION_MAJOR == 4
#include "esp_int_wdt.h"
#include "driver/periph_ctrl.h"      // periph_module_disable/reset on IDF4
#elif ESP_IDF_VERSION_MAJOR == 5
#include "esp_private/esp_int_wdt.h"
#include "esp_private/periph_ctrl.h" // periph_module_disable/reset on IDF5
#endif

#include "src/parsebytes.h"
#include "src/version.h"
#include "camera_pins.h"
#include "storage.h"
#include "myconfig.h"

// ── Global WiFiManager — must be global for loop() wm.process() ──────────────
WiFiManager wm;

// ── NVS ───────────────────────────────────────────────────────────────────────
Preferences devicePrefs;

// ── Subsystem flags ───────────────────────────────────────────────────────────
bool cameraAvailable  = false;
bool sensorsAvailable = false;

// ── Camera retry config ───────────────────────────────────────────────────────
#define MAX_CAMERA_RETRIES     3
#define CAMERA_RETRY_DELAY_MS  2000
#define CAMERA_POWER_SETTLE_MS  500

// ── WiFi portal / sleep config ────────────────────────────────────────────────
#define WIFI_PORTAL_TIMEOUT_SEC  180
#define WIFI_DEEP_SLEEP_SEC       30

// ── NVS keys ──────────────────────────────────────────────────────────────────
#define NVS_NS            "acusky"
#define NVS_CAM_NAME      "cam_name"
#define NVS_SKIP_CAM_ONCE "skip_cam"

// ── Identity ──────────────────────────────────────────────────────────────────
#if defined(CAM_NAME)
char myName[64] = CAM_NAME;
#else
char myName[64] = "AcuSky";
#endif

#if defined(MDNS_NAME)
char mdnsName[] = MDNS_NAME;
#else
char mdnsName[] = "acusky-cam";
#endif

// ── Network ───────────────────────────────────────────────────────────────────
IPAddress ip, net, gw;
char httpURL[64]   = {"Undefined"};
char streamURL[64] = {"Undefined"};

// ── Ports ─────────────────────────────────────────────────────────────────────
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

// ── Camera ────────────────────────────────────────────────────────────────────
// Named 'config' to match extern camera_config_t config; in app_httpd.cpp
camera_config_t config;
int sensorPID = 0;

#if !defined(XCLK_FREQ_MHZ)
unsigned long xclk = 8;
#else
unsigned long xclk = XCLK_FREQ_MHZ;
#endif

#if !defined(CAM_ROTATION)
#define CAM_ROTATION 0
#endif
int myRotation = CAM_ROTATION;

#if !defined(MIN_FRAME_TIME)
#define MIN_FRAME_TIME 0
#endif
int minFrameTime = MIN_FRAME_TIME;

// ── Lamp ──────────────────────────────────────────────────────────────────────
#if defined(LAMP_DISABLE)
int lampVal = -1;
#elif defined(LAMP_PIN)
  #if defined(LAMP_DEFAULT)
  int lampVal = constrain(LAMP_DEFAULT, 0, 100);
  #else
  int lampVal = 0;
  #endif
#else
int lampVal = -1;
#endif
bool autoLamp           = false;
const int lampChannel   = 7;
const int pwmfreq       = 50000;
const int pwmresolution = 9;
const int pwmMax        = pow(2, pwmresolution) - 1;

// ── Filesystem ────────────────────────────────────────────────────────────────
#if defined(NO_FS)
bool filesystem = false;
#else
bool filesystem = true;
#endif

// ── OTA ───────────────────────────────────────────────────────────────────────
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

// ── NTP ───────────────────────────────────────────────────────────────────────
#if defined(NTPSERVER)
bool haveTime             = true;
const char* ntpServer     = NTPSERVER;
const long  gmtOffset_sec = NTP_GMT_OFFSET;
const int   dstOffset_sec = NTP_DST_OFFSET;
#else
bool haveTime             = false;
const char* ntpServer     = "";
const long  gmtOffset_sec = 0;
const int   dstOffset_sec = 0;
#endif

// ── Sketch info ───────────────────────────────────────────────────────────────
int sketchSize;
int sketchSpace;
String sketchMD5;
char myVer[] PROGMEM = __DATE__ " @ " __TIME__;

// ── Stream counters ───────────────────────────────────────────────────────────
int8_t streamCount          = 0;
unsigned long streamsServed  = 0;
unsigned long imagesServed   = 0;

// ── Error string for UI ───────────────────────────────────────────────────────
String critERR = "";

// ── Debug ─────────────────────────────────────────────────────────────────────
bool debugData = false;

// ── Sensors ───────────────────────────────────────────────────────────────────
#define HAS_SENSORS
#if defined(HAS_SENSORS)
#include "src/bme280/BME280I2C.h"
#include "src/aht10/AHT10.h"
#include <Wire.h>
#define I2C_SDA 14
#define I2C_SCL 15
BME280I2C::Settings bmeSettings(
    BME280::OSR_X1, BME280::OSR_X1, BME280::OSR_X1,
    BME280::Mode_Forced, BME280::StandbyTime_1000ms,
    BME280::Filter_Off, BME280::SpiEnable_False,
    BME280I2C::I2CAddr_0x76);
BME280I2C bme(bmeSettings);
AHT10 esp32_aht10(AHT10_ADDRESS_0X38);
#endif

// ── External declarations (app_httpd.cpp) ─────────────────────────────────────
extern void startCameraServer(int hPort, int sPort);
extern void serialDump();

// =============================================================================
// Utilities
// =============================================================================

void flashLED(int ms) {
#if defined(LED_PIN)
    digitalWrite(LED_PIN, LED_ON);
    delay(ms);
    digitalWrite(LED_PIN, LED_OFF);
#endif
}

void setLamp(int newVal) {
#if defined(LAMP_PIN)
    if (newVal != -1) {
        int b = round((pow(2, (1 + (newVal * 0.02))) - 2) / 6 * pwmMax);
        ledcWrite(lampChannel, b);
    }
#endif
}

void calcURLs() {
#if defined(URL_HOSTNAME)
    if (httpPort != 80) sprintf(httpURL,   "http://%s:%d/", URL_HOSTNAME, httpPort);
    else                sprintf(httpURL,   "http://%s/",    URL_HOSTNAME);
    sprintf(streamURL, "http://%s:%d/", URL_HOSTNAME, streamPort);
#else
    if (httpPort != 80) sprintf(httpURL,   "http://%d.%d.%d.%d:%d/", ip[0],ip[1],ip[2],ip[3], httpPort);
    else                sprintf(httpURL,   "http://%d.%d.%d.%d/",    ip[0],ip[1],ip[2],ip[3]);
    sprintf(streamURL, "http://%d.%d.%d.%d:%d/", ip[0],ip[1],ip[2],ip[3], streamPort);
#endif
}

void printLocalTime(bool extra = false) {
    struct tm t;
    if (!getLocalTime(&t)) { Serial.println("Time: unavailable"); return; }
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S, %A, %B %d %Y", &t);
    Serial.println(timeBuf);
    if (extra) Serial.printf("NTP: %s  GMT+%lis  DST+%is\n",
                             ntpServer, gmtOffset_sec, dstOffset_sec);
}

void debugOn()  { debugData = true;  Serial.println("Debug: ON"); }
void debugOff() { debugData = false; Serial.println("Debug: OFF"); }

void handleSerial() {
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'd') serialDump();
        else { if (debugData) debugOff(); else debugOn(); }
    }
    while (Serial.available()) Serial.read();
}

// =============================================================================
// NVS helpers
// =============================================================================

void loadDevicePrefs() {
    devicePrefs.begin(NVS_NS, true);
    String n = devicePrefs.getString(NVS_CAM_NAME, "");
    if (n.length() > 0) n.toCharArray(myName, sizeof(myName));
    devicePrefs.end();
}

void saveDevicePrefs(const char* name) {
    devicePrefs.begin(NVS_NS, false);
    devicePrefs.putString(NVS_CAM_NAME, name);
    devicePrefs.end();
}

void setSkipCameraFlag(bool skip) {
    devicePrefs.begin(NVS_NS, false);
    devicePrefs.putBool(NVS_SKIP_CAM_ONCE, skip);
    devicePrefs.end();
}

bool getSkipCameraFlag() {
    devicePrefs.begin(NVS_NS, true);
    bool v = devicePrefs.getBool(NVS_SKIP_CAM_ONCE, false);
    devicePrefs.end();
    return v;
}

// =============================================================================
// STAGE 0 — Power settle + brownout detection
// =============================================================================

void stage0_powerSettle() {
    Serial.println("\n=== STAGE 0: Power Settle ===");

    // Let the PSU fully stabilise before doing anything.
    // Important after deep-sleep wake — capacitor recharge can look like brownout.
    delay(300);

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("Reset reason: %d  ", reason);

    bool isBrownout = (reason == ESP_RST_BROWNOUT);

    switch(reason) {
        case ESP_RST_POWERON:   Serial.println("(cold boot)");         break;
        case ESP_RST_SW:        Serial.println("(software reset)");    break;
        case ESP_RST_DEEPSLEEP: Serial.println("(deep sleep wake)");   break;
        case ESP_RST_BROWNOUT:  Serial.println("(BROWNOUT)");          break;
        case ESP_RST_WDT:       Serial.println("(watchdog — possible brownout)"); break;
        default:                Serial.printf ("(code %d)\n", reason); break;
    }

    if (isBrownout) {
        // Camera inrush caused a voltage sag. Set NVS flag so stage3 skips
        // camera this boot, giving the PSU a full boot cycle to recover.
        Serial.println(">> BROWNOUT detected. Camera will be skipped this boot.");
        Serial.println("   It will be retried automatically next boot.");
        setSkipCameraFlag(true);
        critERR  = "<h1>PSU Recovery Boot</h1><hr>";
        critERR += "<p>Last reset was caused by a voltage brownout (power sag). ";
        critERR += "Camera is skipped this boot to protect the power supply. ";
        critERR += "It retries automatically on the next reboot.</p>";
        critERR += "<p>WiFi, OTA firmware updates, and sensors remain active.</p>";
    } else {
        bool skipFlagWasSet = getSkipCameraFlag();
        if (skipFlagWasSet) {
            // Previous boot was a brownout. This is the recovery boot.
            // Clear the flag — camera will be attempted normally.
            Serial.println(">> Previous brownout flag cleared. Camera will be attempted.");
            setSkipCameraFlag(false);
        } else {
            Serial.println(">> Normal boot. All subsystems will be attempted.");
        }
    }
}

// =============================================================================
// STAGE 1 — WiFi
// =============================================================================

void stage1_wifi() {
    Serial.println("\n=== STAGE 1: WiFi ===");
    flashLED(200); delay(100); flashLED(200);

    loadDevicePrefs();

    WiFiManagerParameter param_cam_name(NVS_CAM_NAME, "Camera Name", myName, 63);
    wm.addParameter(&param_cam_name);
    wm.setDebugOutput(false);
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);
    // Required for wm.process() non-blocking reconnect in loop()
    wm.setConfigPortalBlocking(false);

    // Start at reduced TX power. WiFi initial connect causes a current spike.
    // We bring it back to full after a stable connection is established.
    WiFi.setTxPower(WIFI_POWER_11dBm);

    wm.setAPCallback([](WiFiManager* w) {
        Serial.printf("Config portal: SSID='AcuSky-Setup'  IP=%s\n",
                      WiFi.softAPIP().toString().c_str());
        for (int i = 0; i < 8; i++) { flashLED(80); delay(80); }
    });

    wm.setSaveParamsCallback([&]() {
        const char* n = param_cam_name.getValue();
        if (strlen(n) > 0) {
            strncpy(myName, n, sizeof(myName) - 1);
            myName[sizeof(myName) - 1] = '\0';
            saveDevicePrefs(myName);
            Serial.printf("Camera name saved: %s\n", myName);
        }
    });

    bool connected = wm.autoConnect("AcuSky-Setup", "acusky123");

    if (!connected) {
        // Not immediately connected — either portal is open waiting for input,
        // or no saved credentials. Poll wm.process() until connected or timeout.
        Serial.println("WiFi not connected — portal may be open. Waiting...");
        unsigned long portalStart = millis();
        unsigned long timeoutMs   = (unsigned long)WIFI_PORTAL_TIMEOUT_SEC * 1000UL;

        while (WiFi.status() != WL_CONNECTED) {
            wm.process();   // serve the portal page
            delay(100);
            if (millis() - portalStart > timeoutMs) {
                // Nobody configured WiFi in time — deep sleep and retry
                Serial.printf("Portal timed out after %ds. Sleeping %ds.\n",
                              WIFI_PORTAL_TIMEOUT_SEC, WIFI_DEEP_SLEEP_SEC);
                flashLED(2000);
                esp_sleep_enable_timer_wakeup(
                    (uint64_t)WIFI_DEEP_SLEEP_SEC * 1000000ULL);
                esp_deep_sleep_start();
            }
        }
    }

    // Restore full TX power once connection is stable
    delay(500);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    ip  = WiFi.localIP();
    net = WiFi.subnetMask();
    gw  = WiFi.gatewayIP();
    calcURLs();
    Serial.printf("Connected: %d.%d.%d.%d  RSSI: %d dBm\n",
                  ip[0],ip[1],ip[2],ip[3], WiFi.RSSI());

    for (int i = 0; i < 5; i++) { flashLED(50); delay(150); }
}

// =============================================================================
// STAGE 2 — OTA / mDNS / NTP
// =============================================================================

void stage2_services() {
    Serial.println("\n=== STAGE 2: OTA / mDNS / NTP ===");

    if (otaEnabled) {
        ArduinoOTA.setHostname(mdnsName);
        if (strlen(otaPassword) > 0) ArduinoOTA.setPassword(otaPassword);

        ArduinoOTA.onStart([]() {
            Serial.println("OTA: starting");
            if (cameraAvailable) { esp_camera_deinit(); cameraAvailable = false; }
            if (lampVal != -1) setLamp(0);
        });
        ArduinoOTA.onEnd([]()   { Serial.println("\nOTA: done"); });
        ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
            Serial.printf("OTA: %u%%\r", (p / (t / 100)));
        });
        ArduinoOTA.onError([](ota_error_t e) {
            const char* msg[] = {"","Auth","Begin","Connect","Receive","End"};
            Serial.printf("OTA Error[%u]: %s failed\n", e, e<=5 ? msg[e] : "?");
        });
        ArduinoOTA.begin();
        Serial.println("OTA: ready");
    }

    if (!MDNS.begin(mdnsName)) {
        Serial.println("WARNING: mDNS start failed (non-fatal)");
    } else {
        MDNS.addService("http", "tcp", httpPort);
        Serial.printf("mDNS: http://%s.local/\n", mdnsName);
    }

    if (haveTime) {
        configTime(gmtOffset_sec, dstOffset_sec, ntpServer);
        Serial.print("NTP: ");
        printLocalTime(true);
    }
}

// =============================================================================
// STAGE 3 — Camera (brownout-aware, retried, non-fatal)
// =============================================================================

// Disable brownout detector for camera init only.
// This is a narrow window around the inrush current peak — NOT a global disable.
// enableBrownout() is ALWAYS called after, even on failure.
static inline void disableBrownout() { WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); }
static inline void enableBrownout()  { WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1); }

static bool attemptCameraInit() {
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = Y2_GPIO_NUM;
    config.pin_d1        = Y3_GPIO_NUM;
    config.pin_d2        = Y4_GPIO_NUM;
    config.pin_d3        = Y5_GPIO_NUM;
    config.pin_d4        = Y6_GPIO_NUM;
    config.pin_d5        = Y7_GPIO_NUM;
    config.pin_d6        = Y8_GPIO_NUM;
    config.pin_d7        = Y9_GPIO_NUM;
    config.pin_xclk      = XCLK_GPIO_NUM;
    config.pin_pclk      = PCLK_GPIO_NUM;
    config.pin_vsync     = VSYNC_GPIO_NUM;
    config.pin_href      = HREF_GPIO_NUM;
    config.pin_sscb_sda  = SIOD_GPIO_NUM;
    config.pin_sscb_scl  = SIOC_GPIO_NUM;
    config.pin_pwdn      = PWDN_GPIO_NUM;
    config.pin_reset     = RESET_GPIO_NUM;
    // Conservative XCLK = lower current spike at startup
    // Increase toward 20MHz once power supply is confirmed stable
    config.xclk_freq_hz  = xclk * 1000000;
    config.pixel_format  = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count     = 2;
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
    }

#if defined(CAMERA_MODEL_ESP_EYE)
    pinMode(13, INPUT_PULLUP);
    pinMode(14, INPUT_PULLUP);
#endif

    // ── Critical window ───────────────────────────────────────────────────────
    // Camera + WiFi TX simultaneously = worst-case current spike.
    // 1. Reduce WiFi TX to minimum
    // 2. Wait for PSU to settle
    // 3. Disable brownout detector just for init
    // 4. Init camera
    // 5. Re-enable brownout detector immediately
    // 6. Restore WiFi TX power
    WiFi.setTxPower(WIFI_POWER_7dBm);
    delay(CAMERA_POWER_SETTLE_MS);
    disableBrownout();

    esp_err_t err = esp_camera_init(&config);

    enableBrownout();                      // ALWAYS re-enable
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // ALWAYS restore TX

    if (err != ESP_OK) {
        Serial.printf("  Camera init error: 0x%04x\n", err);
        // Reset I2C bus so sensors can still initialise cleanly
        periph_module_disable(PERIPH_I2C0_MODULE);
        periph_module_disable(PERIPH_I2C1_MODULE);
        periph_module_reset(PERIPH_I2C0_MODULE);
        periph_module_reset(PERIPH_I2C1_MODULE);
        return false;
    }
    return true;
}

void stage3_camera() {
    Serial.println("\n=== STAGE 3: Camera ===");

    // Brownout flag set in stage0 means skip camera this boot
    if (getSkipCameraFlag()) {
        Serial.println("Camera SKIPPED (brownout recovery boot). Retries next boot.");
        cameraAvailable = false;
        return;
    }

    if (!psramFound()) {
        Serial.println("No PSRAM found — max resolution will be SVGA.");
    }

    bool ok = false;
    for (int attempt = 1; attempt <= MAX_CAMERA_RETRIES && !ok; attempt++) {
        Serial.printf("Camera init: attempt %d/%d\n", attempt, MAX_CAMERA_RETRIES);
        ok = attemptCameraInit();
        if (!ok && attempt < MAX_CAMERA_RETRIES) {
            Serial.printf("Retry in %dms...\n", CAMERA_RETRY_DELAY_MS);
            delay(CAMERA_RETRY_DELAY_MS);
        }
    }

    if (!ok) {
        Serial.println("Camera UNAVAILABLE after all retries. Continuing without.");
        cameraAvailable = false;
        if (critERR.length() == 0) {
            critERR  = "<h1>Camera Unavailable</h1><hr>";
            critERR += "<p>Camera failed after " + String(MAX_CAMERA_RETRIES) + " attempts. ";
            critERR += "Check power supply (5V 1A+) and ribbon cable.</p>";
            critERR += "<p>WiFi, OTA updates, and sensor data remain active.</p>";
        }
        return;
    }

    cameraAvailable = true;
    sensor_t* s = esp_camera_sensor_get();
    sensorPID = s->id.PID;
    Serial.printf("Camera OK. Sensor PID: 0x%x\n", sensorPID);

    if (sensorPID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }
#if defined(CAMERA_MODEL_M5STACK_WIDE)
    s->set_vflip(s, 1); s->set_hmirror(s, 1);
#endif
#if defined(H_MIRROR)
    s->set_hmirror(s, H_MIRROR);
#endif
#if defined(V_FLIP)
    s->set_vflip(s, V_FLIP);
#endif
#if defined(DEFAULT_RESOLUTION)
    s->set_framesize(s, DEFAULT_RESOLUTION);
#else
    s->set_framesize(s, FRAMESIZE_SVGA);
#endif
}

// =============================================================================
// STAGE 4 — Sensors (non-fatal)
// =============================================================================

void stage4_sensors() {
    Serial.println("\n=== STAGE 4: Sensors ===");

#if defined(HAS_SENSORS)
    // Fresh I2C start — safe even if camera init reset the bus
    Wire.begin(I2C_SDA, I2C_SCL);
    delay(50);

    if (!bme.begin()) {
        Serial.println("BME280 not found. Sensors UNAVAILABLE.");
        sensorsAvailable = false;
        return;
    }

    switch (bme.chipModel()) {
        case BME280::ChipModel_BME280:
            Serial.println("BME280: temp + humidity + pressure"); break;
        case BME280::ChipModel_BMP280:
            Serial.println("BMP280: temp + pressure (no humidity)"); break;
        default:
            Serial.println("BME: unknown chip");
    }
    bmeSettings.tempOSR = BME280::OSR_X4;
    bme.setSettings(bmeSettings);

    delay(AHT10_POWER_ON_DELAY);
    Wire.setClock(100000);
    esp32_aht10.softReset();
    Serial.println("AHT10: OK");

    sensorsAvailable = true;
    Serial.println("Sensors: OK.");
#else
    Serial.println("HAS_SENSORS not defined — skipped.");
    sensorsAvailable = false;
#endif
}

// =============================================================================
// Sensor getters (safe to call; check sensorsAvailable first)
// =============================================================================

float getBME280_hum() {
#if defined(HAS_SENSORS)
    if (!sensorsAvailable) return 0;
    return (esp32_aht10.readRawData() != AHT10_ERROR)
           ? esp32_aht10.readTemperature(AHT10_USE_READ_DATA) : 0;
#else
    return 0;
#endif
}

float getBME280_temp() {
#if defined(HAS_SENSORS)
    if (!sensorsAvailable) return 0;
    return bme.temp(BME280::TempUnit_Celsius);
#else
    return 0;
#endif
}

float getBME280_pres() {
#if defined(HAS_SENSORS)
    if (!sensorsAvailable) return 0;
    return bme.pres(BME280::PresUnit_hPa);
#else
    return 0;
#endif
}

// =============================================================================
// SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println("\n============================");
    Serial.printf("AcuSky  |  %s\n", myName);
    Serial.printf("Build:  %s\n", myVer);
    Serial.printf("Base:   %s\n", baseVersion);
    Serial.println("============================");

#if defined(LED_PIN)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_ON);
#endif

    if (filesystem) { filesystemStart(); delay(100); }

    stage0_powerSettle();   // detect brownout, set skip flag if needed
    stage1_wifi();          // must succeed; portal → deep sleep if not
    stage2_services();      // OTA/mDNS/NTP — recovery services
    stage3_camera();        // non-fatal; skipped if brownout boot
    if (cameraAvailable && filesystem) { delay(100); loadPrefs(SPIFFS); }

    // Lamp
    if (lampVal != -1) {
#if defined(LAMP_PIN)
  #if ESP_IDF_VERSION_MAJOR == 4
        ledcSetup(lampChannel, pwmfreq, pwmresolution);
        ledcAttachPin(LAMP_PIN, lampChannel);
  #elif ESP_IDF_VERSION_MAJOR == 5
        ledcAttachChannel(LAMP_PIN, pwmfreq, pwmresolution, lampChannel);
  #endif
        setLamp(autoLamp ? 0 : lampVal);
#endif
    }

    stage4_sensors();       // non-fatal

    // Stage 5: HTTP server — always starts regardless of subsystem state
    sketchSize  = ESP.getSketchSize();
    sketchSpace = ESP.getFreeSketchSpace();
    sketchMD5   = ESP.getSketchMD5();
    startCameraServer(httpPort, streamPort);

    Serial.println("\n============================");
    Serial.println("AcuSky Boot Complete");
    Serial.printf("  WiFi:    %d.%d.%d.%d  (%d dBm)\n",
                  ip[0],ip[1],ip[2],ip[3], WiFi.RSSI());
    Serial.printf("  Camera:  %s\n", cameraAvailable  ? "OK" : "UNAVAILABLE");
    Serial.printf("  Sensors: %s\n", sensorsAvailable ? "OK" : "UNAVAILABLE");
    Serial.printf("  URL:     %s\n", httpURL);
    Serial.printf("  Stream:  %sview\n", streamURL);
    Serial.printf("  Heap:    %u bytes free\n", ESP.getFreeHeap());
    Serial.println("============================\n");

    flashLED(500);

#if defined(DEBUG_DEFAULT_ON)
    debugOn();
#else
    debugOff();
#endif
    while (Serial.available()) Serial.read();
}

// =============================================================================
// LOOP
// =============================================================================

void loop() {
    if (otaEnabled) ArduinoOTA.handle();

    wm.process();   // WiFiManager handles reconnection non-blocking

    handleSerial();

    static bool wifiWasUp = true;
    bool wifiNow = (WiFi.status() == WL_CONNECTED);
    if (wifiWasUp && !wifiNow) {
        Serial.println("WiFi disconnected — reconnecting...");
        wifiWasUp = false;
    } else if (!wifiWasUp && wifiNow) {
        ip = WiFi.localIP(); calcURLs();
        Serial.printf("WiFi reconnected: %d.%d.%d.%d\n", ip[0],ip[1],ip[2],ip[3]);
        wifiWasUp = true;
    }

    delay(100);
}
