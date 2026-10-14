#include "WifiAp.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>

static uint8_t pinned[6] = {};
static bool locked = false;
static bool recovery = false;
static unsigned long disconnectedAt = 0;

void wifiApLoad() {
    Preferences prefs;
    locked = false;
    if (prefs.begin("wifiap", true)) {
        locked = prefs.getBytesLength("bssid") == sizeof(pinned) &&
                 prefs.getBytes("bssid", pinned, sizeof(pinned)) == sizeof(pinned);
        prefs.end();
    }
}

bool wifiApSave(const uint8_t *bssid) {
    Preferences prefs;
    if (!prefs.begin("wifiap", false)) return false;
    bool ok = bssid ? prefs.putBytes("bssid", bssid, sizeof(pinned)) == sizeof(pinned)
                    : (!prefs.isKey("bssid") || prefs.remove("bssid"));
    prefs.end();
    if (ok) {
        locked = bssid != nullptr;
        if (bssid) memcpy(pinned, bssid, sizeof(pinned));
    }
    return ok;
}

const uint8_t *wifiApPinned() { return locked ? pinned : nullptr; }

String wifiApPinnedText() {
    if (!locked) return String();
    char text[18];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             pinned[0], pinned[1], pinned[2], pinned[3], pinned[4], pinned[5]);
    return String(text);
}

String wifiApSsid() {
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return String();
    char ssid[33] = {};
    memcpy(ssid, config.sta.ssid, 32);
    return String(ssid);
}

bool wifiApBeginPinned() {
    if (!locked) return false;
    WiFi.mode(WIFI_STA);
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK || !config.sta.ssid[0]) return false;
    char ssid[33] = {}, password[65] = {};
    memcpy(ssid, config.sta.ssid, 32);
    memcpy(password, config.sta.password, 64);
    WiFi.begin(ssid, password[0] ? password : nullptr, 0, pinned, true);
    memset(password, 0, sizeof(password));
    disconnectedAt = millis();
    return true;
}

bool wifiApRecoveryActive() { return recovery; }

void wifiApRecoveryLoop() {
    if (WiFi.status() == WL_CONNECTED) {
        disconnectedAt = 0;
        if (recovery) {
            WiFi.softAPdisconnect(true);
            recovery = false;
        }
        return;
    }
    if (!disconnectedAt) disconnectedAt = millis();
    // Keep the recovery network until connected, including after the user unlocks.
    if (locked && !recovery && millis() - disconnectedAt >= 60000UL) {
        recovery = WiFi.softAP("ESP32-RTSP-Mic-AP");
    }
}
