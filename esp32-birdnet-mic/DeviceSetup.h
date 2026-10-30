#pragma once
#include <Arduino.h>

// All calls run on the application loop; no network wait is allowed here.
namespace devicesetup {
void begin(bool legacyRecording);
void loop();
bool complete();
bool defaultRecording();
bool standalone();
bool batteryMonitoring();
bool setBatteryMonitoring(bool enabled);
void currentMode(bool recording);
bool apActive();
bool requestWifiOff(uint32_t delayMs);
bool wifiOffPending();
bool wifiSuspended();
const char* wifiOffError();
void cancelWifiOff();
bool suspendWifiNow();
String apName();
String apPassword(); // Only returned through the protected access-details action.
bool shortenApPassword(); // Explicit action; existing configured keys otherwise remain valid.
String savedSsid();
bool connectionBusy();
bool testCredentials(const String& ssid, const String& password);
const char* connectionState();
const char* connectionError();
bool save(bool recording, bool direct, bool finish, int8_t batteryEnabled = -1);
void reconnect(const uint8_t* bssid = nullptr);
bool reset(bool factory);
}
