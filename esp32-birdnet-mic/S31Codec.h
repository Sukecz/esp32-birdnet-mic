#pragma once
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#include <Wire.h>
// Register sequence adapted from Espressif esp-bsp/components/es8311
// SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// GPIO routes: Function-CoreBoard-1 schematic, page 2.
namespace s31codec {
inline bool write(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x18);
    Wire.write(reg); Wire.write(value);
    return Wire.endTransmission() == 0;
}
inline bool begin(uint32_t rate) {
    // MCLK=256fs, Philips 32-bit ADC output, codec slave, analog microphone.
    if (rate < 8000 || rate > 48000) return false;
    digitalWrite(57, LOW); pinMode(57, OUTPUT); // Speaker PA stays muted.
    if (!Wire.begin(51, 50, 400000)) return false;
    if (!write(0x00, 0x1f)) return false;
    delay(20);
    const uint8_t init[][2] = {
        {0x00,0x00},{0x00,0x80},{0x01,0x3f},{0x02,0x00},
        {0x03,0x10},{0x04,0x10},{0x05,0x00},{0x06,0x03},
        {0x07,0x00},{0x08,0xff},{0x09,0x10},{0x0a,0x10},
        {0x0d,0x01},{0x0e,0x02},{0x12,0x00},{0x13,0x10},
        {0x1c,0x6a},{0x37,0x08},{0x17,0xc8},{0x14,0x1a}
    };
    for (const auto& item : init) if (!write(item[0], item[1])) return false;
    return true;
}
}
#endif
