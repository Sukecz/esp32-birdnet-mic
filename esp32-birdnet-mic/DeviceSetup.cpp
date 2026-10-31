#include "DeviceSetup.h"
#include "SetupConfig.h"
#include "WifiAp.h"
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <ctype.h>
#include <atomic>

namespace devicesetup {
namespace {
Config config;
bool recordingMode = false, active = false, openAp = false;
bool trying = false, queued = false, verified = false;
bool hadConnection = false;
bool suspended = false, suspendPending = false;
const char* suspendError = "";
uint32_t suspendAt = 0;
bool configPersisted = false;
std::atomic<uint32_t> gotIpEpoch{0}, disconnectedEpoch{0};
uint32_t trialIpEpoch = 0, trialDisconnectEpoch = 0;
bool trialNeedsDisconnect = false;
uint32_t offlineAt = 0, attemptAt = 0, retryAt = 0, connectedAt = 0, rekeyAt = 0;
uint32_t modeChangeAt = 0;
uint32_t verifiedAt = 0;
String name, trialSsid, trialPassword;
const char* state = "idle";
const char* error = "";
DNSServer dns;

void generateApKey(Config& value) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static_assert(sizeof(alphabet) - 1 == 32, "Uniform 5-bit AP password alphabet");
    memset(value.apKey, 0, sizeof(value.apKey));
    for (size_t i = 0; i < 8; ++i) value.apKey[i] = alphabet[esp_random() & 31];
}

bool persist(const Config& value) {
    Preferences prefs;
    if (!prefs.begin("setup", false)) return false;
    const bool ok = prefs.putBytes("config", &value, sizeof(value)) == sizeof(value);
    prefs.end();
    return ok;
}
void clearTrial() {
    for (size_t i = 0; i < trialPassword.length(); ++i) trialPassword.setCharAt(i, 0);
    trialPassword = String();
    trialSsid = String();
    verified = false;
}
void startAp() {
    if (suspended || suspendPending) return;
    if (active) return;
    WiFi.enableAP(true);
    // WiFiManager's portal preparation: stop an unconnected station before
    // serving the AP. Keep AP enabled so the radio itself is never stopped.
    if (WiFi.status() != WL_CONNECTED && !connectionBusy()) {
        WiFi.disconnect(false);
        WiFi.enableSTA(false);
    }
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    openAp = !config.complete;
    active = WiFi.softAP(name.c_str(), openAp ? nullptr : config.apKey);
    if (active) dns.start(53, "*", WiFi.softAPIP());
}
void stopAp() {
    dns.stop();
    WiFi.softAPdisconnect(true);
    active = false;
}
void connectSaved(const uint8_t* bssid = nullptr) {
    if (suspended || suspendPending || !wantsStation(config, recordingMode) || trying || queued) return;
    WiFi.disconnect(false);
    WiFi.begin(config.ssid, config.password[0] ? config.password : nullptr, 0,
               bssid ? bssid : wifiApPinned(), true);
    retryAt = millis();
}
}

void begin(bool legacyRecording) {
    suspended = suspendPending = false;
    suspendError = "";
    WiFi.mode(WIFI_STA);
    wifi_config_t legacy = {};
    esp_wifi_get_config(WIFI_IF_STA, &legacy);
    Preferences prefs;
    bool loaded = false, migrated = false, previousRecording = false;
    if (prefs.begin("setup", true)) {
        Config saved;
        loaded = prefs.getBytesLength("config") == sizeof(saved) &&
            prefs.getBytes("config", &saved, sizeof(saved)) == sizeof(saved) && valid(saved);
        if (!loaded && prefs.getBytesLength("config") == sizeof(LegacyConfig)) {
            LegacyConfig old;
            loaded = prefs.getBytes("config", &old, sizeof(old)) == sizeof(old) && migrate(old, saved);
            migrated = loaded;
        }
        if (loaded) config = saved;
        prefs.end();
    }
    if (!loaded) {
        if (prefs.begin("recording", true)) {
            previousRecording = prefs.isKey("settings");
            prefs.end();
        }
        config = Config{};
        config.recording = legacyRecording;
        config.complete = legacy.sta.ssid[0] || previousRecording;
        memcpy(config.ssid, legacy.sta.ssid, 32);
        memcpy(config.password, legacy.sta.password, 64);
        // Random, per-device key. No password derived from public MAC/SSID.
        generateApKey(config);
        configPersisted = persist(config); // Save retries a failed initial write.
    }
    else {
        configPersisted = !migrated;
        // No password has been used by an AP client before setup is completed.
        if (migrated && !config.complete && strlen(config.apKey) > 8) {
            generateApKey(config);
        }
        if (migrated) configPersisted = persist(config);
    }
    memset(&legacy, 0, sizeof(legacy));
    // Trial credentials never replace the SDK's flash-backed legacy config.
    WiFi.persistent(false);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent([](arduino_event_id_t event) {
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) gotIpEpoch.fetch_add(1);
        else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) disconnectedEpoch.fetch_add(1);
    });
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06lX", (unsigned long)(ESP.getEfuseMac() >> 24) & 0xFFFFFFUL);
    name = String("BirdNETMic-") + suffix;
    recordingMode = config.recording;
    offlineAt = millis();
    connectSaved();
    if (!config.complete || config.direct) startAp();
}

bool complete() { return config.complete; }
bool defaultRecording() { return config.recording; }
bool standalone() { return config.direct; }
bool batteryMonitoring() { return config.batteryEnabled; }
bool setBatteryMonitoring(bool enabled) {
    Config next = config;
    next.batteryEnabled = enabled;
    if ((!configPersisted || config.batteryEnabled != next.batteryEnabled) && !persist(next)) return false;
    config = next; configPersisted = true;
    return true;
}
bool apActive() { return active; }
bool wifiOffPending() { return suspendPending; }
bool wifiSuspended() { return suspended; }
const char* wifiOffError() { return suspendError; }
void cancelWifiOff() { suspendPending = false; }
bool requestWifiOff(uint32_t delayMs) {
    if (suspended || suspendPending || connectionBusy()) return false;
    suspendError = "";
    suspendAt = millis() + (delayMs < 250 ? 250 : delayMs);
    suspendPending = true; return true;
}
bool suspendWifiNow() {
    if (suspended) return true;
    suspendPending = false;
    const auto previousMode = WiFi.getMode();
    const bool hadAp = active;
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    WiFi.setAutoReconnect(false);
    const bool ok = WiFi.mode(WIFI_OFF);
    if (ok) {
        dns.stop(); suspended = true; active = false; hadConnection = false;
        suspendError = "";
    } else {
        // A failed stop may have changed part of the driver state. Restore the
        // control connection, including DNS for direct AP users, before retry.
        suspendError = "Could not turn Wi-Fi off. Try again.";
        WiFi.mode(previousMode);
        if (hadAp) { active = false; startAp(); }
        if (wantsStation(config, recordingMode) && WiFi.status() != WL_CONNECTED) connectSaved();
    }
    return ok;
}
String apName() { return name; }
String apPassword() { return String(config.apKey); }
bool canChangeApPassword() {
    return !connectionBusy() && !suspended && !suspendPending;
}
bool setApPassword(const String& password) {
    if (!validApPassword(password.c_str(), password.length())) return false;
    if (password == config.apKey && configPersisted) return true;
    if (!canChangeApPassword()) return false;
    Config next = config;
    memset(next.apKey, 0, sizeof(next.apKey));
    password.toCharArray(next.apKey, sizeof(next.apKey));
    if (!persist(next)) return false;
    config = next;
    configPersisted = true;
    // Send the save acknowledgement before AP clients need to reconnect.
    if (active && !openAp) rekeyAt = millis() + 1500;
    return true;
}
bool shortenApPassword() {
    if (strlen(config.apKey) == 8) return true;
    if (!canChangeApPassword() || (active && WiFi.softAPgetStationNum())) return false;
    Config next = config;
    generateApKey(next);
    if (!persist(next)) return false;
    config = next;
    configPersisted = true;
    if (active && !openAp) { stopAp(); startAp(); }
    return true;
}
String savedSsid() { return String(config.ssid); }
bool connectionBusy() { return trying || queued; }
const char* connectionState() { return state; }
const char* connectionError() { return error; }

void currentMode(bool value) {
    recordingMode = value;
    modeChangeAt = millis() + 500; // Let the HTTP response reach a LAN client first.
}

bool testCredentials(const String& ssid, const String& password) {
    if (suspended || suspendPending) return false;
    if (connectionBusy() || WiFi.scanComplete() == WIFI_SCAN_RUNNING ||
        ssid.length() < 1 || ssid.length() > 32 || password.length() > 64 ||
        (password.length() && password.length() < 8)) return false;
    if (password.length() == 64) {
        for (size_t i = 0; i < 64; ++i) if (!isxdigit((unsigned char)password[i])) return false;
    }
    clearTrial();
    trialSsid = ssid;
    trialPassword = password;
    queued = true;
    state = "queued";
    error = "";
    attemptAt = millis();
    startAp(); // Make fallback available before changing the station connection.
    return true;
}

bool save(bool recording, bool direct, bool finish, int8_t batteryEnabled) {
    if (connectionBusy() || (direct && !recording) || batteryEnabled < -1 || batteryEnabled > 1) return false;
    Config next = config;
    next.recording = recording;
    next.direct = direct;
    if (batteryEnabled >= 0) next.batteryEnabled = batteryEnabled;
    if (!direct && verified) {
        if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != trialSsid) return false;
        memset(next.ssid, 0, sizeof(next.ssid));
        memset(next.password, 0, sizeof(next.password));
        trialSsid.toCharArray(next.ssid, sizeof(next.ssid));
        trialPassword.toCharArray(next.password, sizeof(next.password));
    }
    if (finish) next.complete = 1;
    uint8_t previousPin[6] = {};
    const bool clearPin = verified && !direct && wifiApPinned();
    if (clearPin) {
        memcpy(previousPin, wifiApPinned(), sizeof(previousPin));
        if (!wifiApSave(nullptr)) return false;
    }
    if ((!configPersisted || memcmp(&config, &next, sizeof(config))) && !persist(next)) {
        if (clearPin) wifiApSave(previousPin);
        return false;
    }
    configPersisted = true;
    config = next;
    clearTrial();
    state = "idle";
    error = "";
    if (active && openAp && config.complete) rekeyAt = millis() + 4000;
    return true;
}

void reconnect(const uint8_t* bssid) {
    if (suspended || suspendPending || connectionBusy()) return;
    clearTrial();
    state = "idle";
    connectSaved(bssid);
}

bool reset(bool factory) {
    Config next = factory ? Config{} : config;
    next.complete = 0;
    next.direct = 0;
    memset(next.ssid, 0, sizeof(next.ssid));
    memset(next.password, 0, sizeof(next.password));
    memcpy(next.apKey, config.apKey, sizeof(next.apKey));
    if (!persist(next)) return false;
    configPersisted = true;
    config = next;
    wifi_config_t blank = {};
    WiFi.disconnect(false);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    const bool erased = esp_wifi_set_config(WIFI_IF_STA, &blank) == ESP_OK;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    clearTrial();
    return erased;
}

void loop() {
    if (suspended) return;
    const uint32_t now = millis();
    if (suspendPending && int32_t(now-suspendAt) >= 0) {
        suspendWifiNow(); return;
    }
    if (suspendPending) return;
    if (active) dns.processNextRequest();
    if (modeChangeAt && int32_t(now - modeChangeAt) >= 0 && !connectionBusy()) {
        modeChangeAt = 0;
        if (wantsStation(config, recordingMode)) {
            if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != config.ssid) connectSaved();
        } else if (config.direct && recordingMode) {
            startAp();
            WiFi.disconnect(false);
        }
    }
    if (rekeyAt && int32_t(now - rekeyAt) >= 0) {
        rekeyAt = 0;
        stopAp();
        startAp(); // Apply a saved password or complete open provisioning.
    }
    if (queued && now - attemptAt >= 300 && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        queued = false;
        trying = true;
        state = "connecting";
        attemptAt = now;
        trialNeedsDisconnect = WiFi.status() == WL_CONNECTED;
        trialIpEpoch = gotIpEpoch.load();
        trialDisconnectEpoch = disconnectedEpoch.load();
        WiFi.disconnect(false);
        WiFi.begin(trialSsid.c_str(), trialPassword.length() ? trialPassword.c_str() : nullptr);
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (trying) {
        if (connected && WiFi.SSID() == trialSsid && gotIpEpoch.load() != trialIpEpoch &&
            (!trialNeedsDisconnect || disconnectedEpoch.load() != trialDisconnectEpoch)) {
            trying = false;
            verified = true;
            verifiedAt = now;
            state = "connected";
        } else if (now - attemptAt >= 25000) {
            trying = false;
            state = "error";
            error = "connection_failed";
            clearTrial();
            connectSaved();
        }
    }
    if (verified && (!connected || now - verifiedAt >= 300000)) {
        state = "error";
        error = connected ? "trial_expired" : "connection_lost";
        clearTrial();
        connectSaved();
    }
    if (connected) {
        offlineAt = now;
        if (!hadConnection) connectedAt = now;
    }
    hadConnection = connected;
    if (needsAp(config.complete, config.direct && recordingMode, connected, now - offlineAt)) startAp();
    // Never remove the AP from a connected user, including while LAN returns.
    if (active && !openAp && !rekeyAt && config.complete && !(config.direct && recordingMode) &&
        !connectionBusy() && !verified && connected && now - connectedAt >= 60000 &&
        WiFi.softAPgetStationNum() == 0) stopAp();
    if (!connected && wantsStation(config, recordingMode) && !connectionBusy() &&
        !verified && WiFi.scanComplete() != WIFI_SCAN_RUNNING && now - retryAt >= 30000) connectSaved();
}
}
