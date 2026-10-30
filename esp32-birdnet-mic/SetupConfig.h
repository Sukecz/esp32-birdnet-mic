#pragma once
#include <stdint.h>
#include <string.h>

namespace devicesetup {
// A single NVS blob is the commit boundary, including verified credentials.
struct Config {
    uint32_t magic = 0x53545531;
    // batteryEnabled occupies the formerly zero reserved byte; old configs stay valid.
    uint8_t complete = 0, recording = 0, direct = 0, batteryEnabled = 0;
    char ssid[33] = {}, password[65] = {}, apKey[17] = {};
};
inline bool valid(const Config& c) {
    return c.magic == Config{}.magic && c.complete <= 1 && c.recording <= 1 && c.direct <= 1 &&
        (!c.direct || c.recording) && c.batteryEnabled <= 1 && c.ssid[32] == 0 && c.password[64] == 0 && c.apKey[16] == 0 &&
        (strlen(c.apKey) == 8 || strlen(c.apKey) == 16);
}
inline bool wantsStation(const Config& c, bool currentRecording) {
    return c.ssid[0] && (!c.direct || !currentRecording);
}
inline bool needsAp(bool configured, bool directRecording, bool connected, uint32_t offlineMs) {
    return !configured || directRecording || (!connected && offlineMs >= 15000);
}
}
