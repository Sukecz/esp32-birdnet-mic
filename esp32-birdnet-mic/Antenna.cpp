#include "Antenna.h"
#include <Arduino.h>
#if defined(ARDUINO_XIAO_ESP32C6)
#include <Preferences.h>
#endif

namespace antenna {
namespace {
bool activeExternal = true;
#if defined(ARDUINO_XIAO_ESP32C6)
bool savedExternal = true;
#endif
}
bool supported() {
#if defined(ARDUINO_XIAO_ESP32C6)
    return true;
#else
    return false;
#endif
}
bool external() { return activeExternal; }
void begin() {
#if defined(ARDUINO_XIAO_ESP32C6)
    // Missing settings preserve the previous firmware's external-antenna default.
    Preferences prefs;
    savedExternal = true;
    if (prefs.begin("wifiant", true)) {
        savedExternal = prefs.getBool("external", true);
        prefs.end();
    }
    activeExternal = savedExternal;
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    pinMode(14, OUTPUT);
    digitalWrite(14, activeExternal ? HIGH : LOW);
#endif
}
bool save(bool useExternal) {
#if defined(ARDUINO_XIAO_ESP32C6)
    if (useExternal == savedExternal) return true;
    Preferences prefs;
    if (!prefs.begin("wifiant", false)) return false;
    const bool ok = prefs.putBool("external", useExternal) == 1;
    prefs.end();
    if (ok) savedExternal = useExternal;
    return ok;
#else
    (void)useExternal;
    return false;
#endif
}
}
