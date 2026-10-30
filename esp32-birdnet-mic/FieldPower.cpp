#include "FieldPower.h"
#include <Preferences.h>
#include <string.h>
namespace fieldpower {
namespace {
Settings config;
LowBattery low;
bool saveFinished = false, saveOk = false;
}
void begin() {
    Preferences p;
    if (p.begin("fieldpower", true)) {
        Settings s;
        if (p.getBytesLength("settings") == sizeof(s) && p.getBytes("settings", &s, sizeof(s)) == sizeof(s) && valid(s)) config = s;
        p.end();
    }
}
Settings settings() { return config; }
bool saveSettings(const Settings& s) {
    if (!valid(s)) return false;
    if (!memcmp(&s, &config, sizeof(s))) return true;
    Preferences p;
    if (!p.begin("fieldpower", false)) return false;
    const bool ok = p.putBytes("settings", &s, sizeof(s)) == sizeof(s);
    p.end();
    if (ok) { config = s; low.reset(); saveFinished = saveOk = false; }
    return ok;
}
bool observe(bool active, const battery::Reading& reading) {
    const bool triggeredNow = low.update(config, active, reading.valid, reading.millivolts, reading.sequence, millis());
    if (triggeredNow) saveFinished = saveOk = false;
    return triggeredNow;
}
bool canStart(const battery::Reading& r) { return low.canStart(config, r.valid, r.millivolts); }
bool triggered() { return low.latched; }
bool acknowledge(const battery::Reading& r) {
    if (!low.canAcknowledge(config, r.valid, r.millivolts)) return false;
    low.reset(); saveFinished = saveOk = false; return true;
}
void saved(bool success) { saveFinished = true; saveOk = success; }
String statusJson(const battery::Reading& r) {
    const char* state = !config.guardEnabled ? "disabled" : low.latched ?
        (saveFinished ? (saveOk ? "saved" : "save_failed") : "saving") :
        !r.valid ? "measurement_unavailable" : low.pending ? "low_pending" : "ready";
    String s = "{\"enabled\":" + String(config.guardEnabled ? "true" : "false");
    s += ",\"cutoff_mv\":" + String(config.cutoffMv) + ",\"confirm_seconds\":" + String(config.confirmSeconds);
    s += ",\"hysteresis_mv\":" + String(config.hysteresisMv) + ",\"wifi_delay_seconds\":" + String(config.wifiDelaySeconds);
    s += ",\"sleep_after_save\":" + String(config.sleepAfterSave ? "true" : "false");
    s += ",\"state\":\"" + String(state) + "\",\"latched\":" + String(low.latched ? "true" : "false");
    s += ",\"start_allowed\":" + String(canStart(r) ? "true" : "false");
    s += ",\"can_acknowledge\":" + String(low.latched && low.canAcknowledge(config,r.valid,r.millivolts) ? "true" : "false");
    s += ",\"sample_mv\":" + (r.valid ? String(r.millivolts) : String("null"));
    const uint32_t elapsed = uint32_t(millis()-low.lowAt)/1000;
    const uint32_t remaining = low.pending && !low.latched && elapsed < config.confirmSeconds ? config.confirmSeconds-elapsed : 0;
    s += ",\"remaining_seconds\":" + String(remaining);
    return s + "}";
}
}
