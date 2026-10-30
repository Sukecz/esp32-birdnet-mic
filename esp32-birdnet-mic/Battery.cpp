#include "Battery.h"
#include <Preferences.h>

namespace battery {
namespace {
bool active = false, valid = false, sampled = false, collecting = false;
Pins selected{-1,-1,false};
uint32_t nextStep = 0, lastSample = 0, total = 0, millivolts = 0;
uint8_t count = 0;
uint32_t latestMv = 0, sampleSequence = 0;
uint32_t scale = 10000;
bool scaleLoaded = false;
void loadScale() {
    if (scaleLoaded) return;
    scaleLoaded = true; // An absent NVS namespace means default calibration.
    Preferences prefs;
    if (!prefs.begin("battery", true)) return;
    uint32_t saved = 10000;
    if (prefs.getBytesLength("scale") == sizeof(saved) &&
        prefs.getBytes("scale", &saved, sizeof(saved)) == sizeof(saved) &&
        saved >= 9000 && saved <= 11000) scale = saved;
    prefs.end(); scaleLoaded = true;
}
bool saveScale(uint32_t value) {
    loadScale();
    if (value < 9000 || value > 11000) return false;
    if (value == scale) return true;
    Preferences prefs;
    if (!prefs.begin("battery", false)) return false;
    const bool ok = prefs.putBytes("scale", &value, sizeof(value)) == sizeof(value);
    prefs.end();
    if (ok) { scale = value; scaleLoaded = true; }
    return ok;
}
void gate(bool on) {
    if (selected.enable >= 0) digitalWrite(selected.enable, on ? HIGH : LOW);
}
void clear() {
    gate(false);
    active = valid = sampled = collecting = false;
    total = millivolts = count = 0;
}
}
void loop(bool enabled, uint8_t variant) {
    loadScale();
    const auto target = pins(variant);
    if (!enabled || !target.supported) { if (active) clear(); return; }
    const uint32_t now = millis();
    if (!active || selected.adc != target.adc || selected.enable != target.enable) {
        clear();
        selected = target;
        pinMode(selected.adc, INPUT);
        analogSetPinAttenuation(selected.adc, ADC_11db);
        if (selected.enable >= 0) {
            digitalWrite(selected.enable, LOW);
            pinMode(selected.enable, OUTPUT);
        }
        active = true;
    }
    if (!collecting) {
        if (sampled && uint32_t(now-lastSample) < 10000) return;
        gate(true);
        collecting = true;
        count = 0; total = 0;
        nextStep = now + 20; // Allow the divider / optional input capacitor to settle.
        return;
    }
    if (int32_t(now-nextStep) < 0) return;
    total += analogReadMilliVolts(selected.adc);
    nextStep = now + 2;
    if (++count < 16) return;
    gate(false);
    const uint32_t reading = (total * 2 + 8) / 16;
    const bool plausible = reading >= 2000 && reading <= 4400;
    millivolts = plausible ? (valid ? (millivolts*3 + reading + 2)/4 : reading) : 0;
    latestMv = plausible ? reading : 0; ++sampleSequence;
    valid = plausible;
    sampled = true; collecting = false; lastSample = now;
}
Reading reading(bool enabled, uint8_t variant) {
    loadScale();
    const auto target = pins(variant);
    const bool available = enabled && target.supported && active && valid &&
        selected.adc == target.adc && selected.enable == target.enable && uint32_t(millis()-lastSample) <= 30000;
    return {available, available ? (latestMv*scale+5000)/10000 : 0, sampleSequence};
}
bool calibrate(uint32_t referenceMv, uint8_t variant) {
    const auto target = pins(variant);
    if (!target.supported || !active || !valid ||
        selected.adc != target.adc || selected.enable != target.enable ||
        uint32_t(millis()-lastSample) > 30000 || referenceMv < 2500 || referenceMv > 4300) return false;
    // Always reference the uncorrected measurement, avoiding cumulative scaling.
    return saveScale((referenceMv * 10000 + millivolts/2) / millivolts);
}
bool resetCalibration() { return saveScale(10000); }
String statusJson(bool enabled, uint8_t variant) {
    loadScale();
    const auto target = pins(variant);
    const bool available = enabled && target.supported && active && valid &&
        selected.adc == target.adc && selected.enable == target.enable &&
        uint32_t(millis()-lastSample) <= 30000;
    const uint32_t corrected = (millivolts * scale + 5000) / 10000;
    String json = "{\"enabled\":" + String(enabled ? "true" : "false");
    json += ",\"supported\":" + String(target.supported ? "true" : "false");
    json += ",\"valid\":" + String(available ? "true" : "false");
    json += ",\"voltage\":" + (available ? String(corrected/1000.0f, 2) : String("null"));
    json += ",\"percent\":" + (available ? String(percent(corrected)) : String("null"));
    json += ",\"raw_voltage\":" + (available ? String(millivolts/1000.0f, 3) : String("null"));
    json += ",\"calibration_percent\":" + String((int32_t(scale)-10000)/100.0f, 2);
    json += "}";
    return json;
}
}
