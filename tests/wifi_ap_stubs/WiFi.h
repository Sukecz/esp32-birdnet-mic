#pragma once
#include "Arduino.h"
constexpr int WIFI_STA = 1, WL_CONNECTED = 3;
class TestWiFi {
public:
 int state = 0, apStarts = 0, apStops = 0;
 String begunSsid, begunPassword;
 uint8_t begunBssid[6] = {};
 int status() { return state; }
 void mode(int) {}
 void begin(const char *ssid, const char *password, int, const uint8_t *bssid, bool) {
   begunSsid = ssid; begunPassword = password ? password : ""; memcpy(begunBssid, bssid, 6);
 }
 bool softAP(const char *) { ++apStarts; return true; }
 void softAPdisconnect(bool) { ++apStops; }
};
extern TestWiFi WiFi;
