#pragma once
#include <Arduino.h>
#include "S3Boards.h"

// The XIAO header positions are shared; Arduino maps Dn to each chip's GPIO.
// Adafruit BFF uses TX/D6 for CS and the regular D8/D9/D10 SPI header pins.
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3) || \
    defined(ARDUINO_XIAO_ESP32C5) || defined(ARDUINO_XIAO_ESP32C6)
#define RECORDING_SPI_SUPPORTED 1
#else
#define RECORDING_SPI_SUPPORTED 0
#endif

namespace recordingpins {
struct Pins {
    int cs, sck, miso, mosi;
    bool supported, builtin;
};
// Sense uses the factory SD_CS route (R12 populated, R11 DNP on ExpBoard v1.2).
// Seeed: GPIO21 CS, GPIO7 SCK, GPIO8 MISO, GPIO9 MOSI.
// https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/
inline Pins resolve(uint8_t variant = s3boards::External) {
#if RECORDING_SPI_SUPPORTED
#if defined(ARDUINO_XIAO_ESP32S3)
    if (!s3boards::valid(variant) || variant == s3boards::AtomS3U)
        return {-1, -1, -1, -1, false, false};
    if (variant == s3boards::Sense) return {21, 7, 8, 9, true, true};
#else
    (void)variant; // A saved S3 profile cannot reroute another chip's GPIOs.
#endif
    return {D6, D8, D9, D10, true, false};
#else
    (void)variant;
    return {-1, -1, -1, -1, false, false};
#endif
}
inline const char* storage(uint8_t variant = s3boards::External) {
    const auto pins = resolve(variant);
    return !pins.supported ? "unavailable" : pins.builtin ? "sense_builtin" : "external_spi";
}
}
