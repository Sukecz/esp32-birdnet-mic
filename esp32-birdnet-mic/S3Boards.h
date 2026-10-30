#pragma once
#include <stdint.h>

// Stored values are part of the NVS/API contract. External remains the default.
namespace s3boards {
enum Variant : uint8_t { External = 0, Sense = 1, AtomS3U = 2 };
// All settings changed by a board switch must be restored on setup failure.
struct Config {
    uint8_t variant, format;
    uint32_t rate, minRate;
    uint16_t cutoff;
};
inline bool valid(uint8_t value) { return value <= AtomS3U; }
inline bool builtin(uint8_t value) { return value == Sense || value == AtomS3U; }
inline int clockPin(uint8_t value, int external) {
    return value == Sense ? 42 : value == AtomS3U ? 39 : external;
}
inline int dataPin(uint8_t value, int external) {
    return value == Sense ? 41 : value == AtomS3U ? 38 : external;
}
// AtomS3U pins follow M5Unified and the hardware-tested PR #62, rather than
// the reversed CLK/DATA columns in the M5Stack product table.
inline const char* name(uint8_t value) {
    return value == Sense ? "XIAO ESP32-S3 Sense" :
        value == AtomS3U ? "M5Stack AtomS3U" : "XIAO ESP32-S3";
}
inline const char* id(uint8_t value) {
    return value == Sense ? "xiao-esp32s3-sense" :
        value == AtomS3U ? "m5stack-atoms3u" : "xiao-esp32s3";
}
}
