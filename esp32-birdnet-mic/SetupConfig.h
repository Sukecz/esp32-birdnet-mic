#pragma once
#include <stdint.h>
#include <string.h>

namespace devicesetup {
// A single NVS blob is the commit boundary, including verified credentials.
struct LegacyConfig {
    uint32_t magic = 0x53545531;
    // batteryEnabled occupies the formerly zero reserved byte; old configs stay valid.
    uint8_t complete = 0, recording = 0, direct = 0, batteryEnabled = 0;
    char ssid[33] = {}, password[65] = {}, apKey[17] = {};
};
struct Config {
    uint32_t magic = 0x53545532;
    uint8_t complete = 0, recording = 0, direct = 0, batteryEnabled = 0;
    char ssid[33] = {}, password[65] = {}, apKey[64] = {};
};
inline bool validApPassword(const char* key, size_t length) {
    if (length < 8 || length > 63) return false;
    for (size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(key[i]) < 32 || static_cast<unsigned char>(key[i]) > 126) return false;
    return true;
}
inline bool valid(const Config& c) {
    return c.magic == Config{}.magic && c.complete <= 1 && c.recording <= 1 && c.direct <= 1 &&
        (!c.direct || c.recording) && c.batteryEnabled <= 1 && c.ssid[32] == 0 && c.password[64] == 0 && c.apKey[63] == 0 &&
        validApPassword(c.apKey, strlen(c.apKey));
}
// Decode the old blob explicitly: enlarging the passphrase must never reset Wi-Fi or setup.
inline bool migrate(const LegacyConfig& old, Config& next) {
    if (old.magic != LegacyConfig{}.magic || old.apKey[16] != 0 ||
        (strlen(old.apKey) != 8 && strlen(old.apKey) != 16)) return false;
    next = Config{};
    next.complete = old.complete; next.recording = old.recording;
    next.direct = old.direct; next.batteryEnabled = old.batteryEnabled;
    memcpy(next.ssid, old.ssid, sizeof(old.ssid));
    memcpy(next.password, old.password, sizeof(old.password));
    memcpy(next.apKey, old.apKey, sizeof(old.apKey));
    return valid(next);
}
inline bool wantsStation(const Config& c, bool currentRecording) {
    return c.ssid[0] && (!c.direct || !currentRecording);
}
inline bool needsAp(bool configured, bool directRecording, bool connected, uint32_t offlineMs) {
    return !configured || directRecording || (!connected && offlineMs >= 15000);
}
}
