#pragma once
#include <stdint.h>

// One source for the API preview and the values applied by firmware.
namespace micprofiles {
enum Board : uint8_t { Unknown, C3, S3, C5, C6, S31 };
struct Settings {
    uint32_t rate;
    float gain;
    uint16_t buffer;
    uint8_t shift;
    bool highpass;
    uint16_t cutoff;
    uint16_t cpu; // zero: preserve CPU setting
};

inline bool recommended(Board board, uint8_t format, Settings& out) {
    if (board == Unknown || board > S31 || format > 2) return false;
    if (board == S31 && format != 0) return false;
    // 512 is also a safe common starting point for BirdNET-Pi UDP.
    out = {48000, 1.5f, 512, 12, true, 500, 0};
    if (format == 2) {
        // Dan's integrated-firmware report: old Shift 12 / Gain 1.5 -> Gain 24.
        // Starting level only; microphone sensitivity and placement still matter.
        out.gain = 24.0f;
        if (board != S3) out.cpu = 160; // software PDM; S3 converts in hardware
    }
    return true;
}
} // namespace micprofiles
