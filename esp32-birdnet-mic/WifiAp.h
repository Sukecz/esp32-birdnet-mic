#pragma once
#include <Arduino.h>

void wifiApLoad();
bool wifiApSave(const uint8_t *bssid);
const uint8_t *wifiApPinned();
String wifiApPinnedText();
String wifiApSsid();
bool wifiApBeginPinned();
void wifiApRecoveryLoop();
bool wifiApRecoveryActive();
