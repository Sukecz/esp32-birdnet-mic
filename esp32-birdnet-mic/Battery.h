#pragma once
#include <Arduino.h>
#include "S3Boards.h"

namespace battery {
struct Pins { int adc, enable; bool supported; };
inline Pins pins(uint8_t variant = s3boards::External) {
#if defined(ARDUINO_XIAO_ESP32C5)
    (void)variant;
    return {BAT_VOLT_PIN, BAT_VOLT_PIN_EN, true};
#elif defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6)
    (void)variant;
    return {A0, -1, true};
#elif defined(ARDUINO_XIAO_ESP32S3)
    return {A0, -1, variant == s3boards::External || variant == s3boards::Sense};
#else
    (void)variant;
    return {-1, -1, false};
#endif
}
// Voltage-only estimate for a 4.2 V Li-ion/LiPo 1S pack, including parallel cells.
// These generic points are not a fuel gauge or a battery protection threshold.
inline uint8_t percent(uint32_t mv) {
    static constexpr uint16_t voltage[] = {3300,3500,3600,3700,3750,3800,3850,3900,4000,4100,4200};
    static constexpr uint8_t charge[] = {0,5,10,20,30,40,50,60,75,90,100};
    if (mv <= voltage[0]) return 0;
    for (size_t i = 1; i < sizeof(voltage)/sizeof(voltage[0]); ++i)
        if (mv < voltage[i]) return charge[i-1] +
            (mv-voltage[i-1])*(charge[i]-charge[i-1])/(voltage[i]-voltage[i-1]);
    return 100;
}
// Called only from the application loop; one ADC conversion per step, no delays.
struct Reading { bool valid; uint32_t millivolts, sequence; };
// Fresh calibrated batch average (not display smoothing) for low-voltage control.
Reading reading(bool enabled, uint8_t variant);
void loop(bool enabled, uint8_t variant);
bool calibrate(uint32_t referenceMv, uint8_t variant);
bool resetCalibration();
String statusJson(bool enabled, uint8_t variant);
}
