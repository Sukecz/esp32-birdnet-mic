#pragma once
#include <stdint.h>
#include <stddef.h>
namespace fieldpower {
struct Settings {
    uint32_t magic = 0x46505731;
    uint16_t cutoffMv = 3500;
    uint16_t confirmSeconds = 30;
    uint16_t hysteresisMv = 100;
    uint8_t wifiDelaySeconds = 3;
    uint8_t guardEnabled = 0;
    uint8_t sleepAfterSave = 1;
    uint8_t reserved[3] = {};
};
static_assert(sizeof(Settings) == 16, "Persistent field power settings layout");
inline bool valid(const Settings& s) {
    return s.magic == 0x46505731 && s.cutoffMv >= 3000 && s.cutoffMv <= 4200 &&
        s.confirmSeconds <= 300 && s.hysteresisMv <= 300 &&
        s.cutoffMv + s.hysteresisMv <= 4500 && s.wifiDelaySeconds <= 60 &&
        s.guardEnabled <= 1 && s.sleepAfterSave <= 1;
}
struct LowBattery {
    bool pending = false, latched = false;
    bool seenSample = false;
    uint32_t lowAt = 0, sequence = 0;
    void reset() { *this = LowBattery{}; }
    bool update(const Settings& s, bool active, bool measured, uint32_t mv, uint32_t sample, uint32_t now) {
        if (!s.guardEnabled) { reset(); return false; }
        if (latched) return false;
        if (!active || !measured) { pending = false; seenSample = false; return false; }
        if (seenSample && sequence == sample) return false;
        seenSample = true; sequence = sample;
        if (mv > uint32_t(s.cutoffMv) + s.hysteresisMv) pending = false;
        if (mv <= s.cutoffMv) {
            if (!pending) { pending = true; lowAt = now; }
            if (uint32_t(now-lowAt) >= uint32_t(s.confirmSeconds)*1000) {
                latched = true; return true;
            }
        }
        return false;
    }
    bool canStart(const Settings& s, bool measured, uint32_t mv) const {
        return !s.guardEnabled || (!latched && measured && mv > s.cutoffMv);
    }
    bool canAcknowledge(const Settings& s, bool measured, uint32_t mv) const {
        return !s.guardEnabled || (measured && mv > uint32_t(s.cutoffMv)+s.hysteresisMv);
    }
};
}
