/*
 * app_httpd.cpp — Patch Guide for WiFiManager + Brownout Integration
 * ===================================================================
 * Apply these targeted changes to your existing app_httpd.cpp.
 * Everything else in the file stays the same.
 *
 * Summary of changes:
 *   1. Add includes and extern declarations
 *   2. Add reset_wifi command to cmd_handler()
 *   3. Guard readSensor_handler() with sensorsAvailable
 *   4. Guard stream/capture handlers with cameraAvailable
 *   5. Add subsystem status to dump_handler()
 */


// ─────────────────────────────────────────────────────────────────────────────
// CHANGE 1 — Add at the top of app_httpd.cpp, with existing includes
// ─────────────────────────────────────────────────────────────────────────────

#include <WiFiManager.h>

// Replace the old accesspoint/captivePortal externs with these:
extern bool cameraAvailable;
extern bool sensorsAvailable;

// Remove these old externs (no longer used):
//   extern bool accesspoint;
//   extern char apName[];
//   extern bool captivePortal;


// ─────────────────────────────────────────────────────────────────────────────
// CHANGE 2 — Add reset_wifi to cmd_handler()
//
// Find the block that handles the "reboot" variable and ADD this BEFORE it.
// ─────────────────────────────────────────────────────────────────────────────

/*
    else if (!strcmp(variable, "reset_wifi")) {
        // Erase stored WiFi credentials from NVS.
        // Device reboots into AcuSky-Setup config portal.
        Serial.println("WiFi reset requested via web UI");

        // Turn off lamp before reboot
        if (lampVal != -1) setLamp(0);

        // Send HTTP response before rebooting so the browser gets an answer
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, "WiFi credentials cleared. Connect to 'AcuSky-Setup' to reconfigure.");

        delay(500);
        WiFiManager wm;
        wm.resetSettings();   // clears NVS WiFi credentials
        ESP.restart();
        return ESP_OK;
    }
*/


// ─────────────────────────────────────────────────────────────────────────────
// CHANGE 3 — Guard readSensor_handler() with sensorsAvailable
//
// Replace the existing readSensor_handler body with this.
// ─────────────────────────────────────────────────────────────────────────────

/*
static esp_err_t readSensor_handler(httpd_req_t *req) {
    flashLED(75);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!sensorsAvailable) {
        // Return a clearly-marked unavailable response so the UI can show it.
        // Format matches normal response so JS parsing doesn't break.
        return httpd_resp_sendstr(req, "0#0#0#unavailable");
    }

    float hum  = getBME280_hum();
    float temp = getBME280_temp();
    float pres = getBME280_pres();

    String s = String(hum) + '#' + String(temp) + '#' + String(pres) + '#';
    int len = s.length() + 1;
    char buf[len];
    s.toCharArray(buf, len);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}
*/


// ─────────────────────────────────────────────────────────────────────────────
// CHANGE 4 — Guard stream and capture handlers with cameraAvailable
//
// At the TOP of stream_handler() and capture_handler(), add this early return:
// ─────────────────────────────────────────────────────────────────────────────

/*
    // In stream_handler():
    if (!cameraAvailable) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "Camera unavailable. Check power supply and ribbon cable.");
    }

    // In capture_handler():
    if (!cameraAvailable) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "Camera unavailable. Check power supply and ribbon cable.");
    }
*/


// ─────────────────────────────────────────────────────────────────────────────
// CHANGE 5 — Add subsystem status to dump_handler()
//
// In dump_handler(), after the existing WiFi info section, add:
// ─────────────────────────────────────────────────────────────────────────────

/*
    // Subsystem health
    d += sprintf(d, "<h2>Subsystems</h2>\n");
    d += sprintf(d,
        "Camera: <b style=\"color:%s\">%s</b><br>\n",
        cameraAvailable  ? "green" : "red",
        cameraAvailable  ? "OK"    : "Unavailable &mdash; check power supply and ribbon cable");
    d += sprintf(d,
        "Sensors: <b style=\"color:%s\">%s</b><br>\n",
        sensorsAvailable ? "green" : "red",
        sensorsAvailable ? "OK"    : "Unavailable &mdash; check I2C wiring (SDA=GPIO14, SCL=GPIO15)");

    // Last reset reason (helpful for diagnosing brownouts)
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "Unknown";
    switch(reason) {
        case ESP_RST_POWERON:   reasonStr = "Power-on";        break;
        case ESP_RST_SW:        reasonStr = "Software reset";  break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT";        break;
        case ESP_RST_WDT:       reasonStr = "Watchdog";        break;
        case ESP_RST_DEEPSLEEP: reasonStr = "Deep sleep wake"; break;
        default: break;
    }
    d += sprintf(d,
        "Last reset reason: <b style=\"color:%s\">%s</b><br>\n",
        reason == ESP_RST_BROWNOUT ? "red" : "black",
        reasonStr);
*/
