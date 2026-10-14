// Build: c++ -std=c++17 -Itests/wifi_ap_stubs tests/wifi_ap_test.cpp esp32-birdnet-mic/WifiAp.cpp -o /tmp/wifi_ap_test
#include <cassert>
#include <vector>
#include "../esp32-birdnet-mic/WifiAp.h"
#include "WiFi.h"
#include "esp_wifi.h"
unsigned long testNow = 100;
std::vector<uint8_t> testSaved;
bool testWriteFails = false;
wifi_config_t testConfig = {};
TestWiFi WiFi;
int main() {
  wifiApLoad();
  assert(!wifiApPinned() && !wifiApBeginPinned());
  const uint8_t chosen[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
  assert(wifiApSave(chosen));
  wifiApLoad(); // A reboot must reload the exact selection.
  assert(wifiApPinnedText() == "AA:BB:CC:11:22:33");
  assert(!wifiApBeginPinned()); // No credentials: initial setup remains possible.
  // Maximum-length SSID/password must be safely terminated.
  memset(testConfig.sta.ssid, 's', 32);
  memset(testConfig.sta.password, 'p', 64);
  assert(wifiApSsid().size() == 32);
  assert(wifiApBeginPinned());
  assert(WiFi.begunSsid.size() == 32 && WiFi.begunPassword.size() == 64);
  assert(memcmp(WiFi.begunBssid, chosen, 6) == 0);
  testNow += 59999;
  wifiApRecoveryLoop();
  assert(!wifiApRecoveryActive());
  ++testNow; wifiApRecoveryLoop();
  assert(wifiApRecoveryActive() && WiFi.apStarts == 1);
  wifiApRecoveryLoop(); assert(WiFi.apStarts == 1);
  testWriteFails = true;
  assert(!wifiApSave(nullptr) && wifiApPinned());
  testWriteFails = false;
  assert(wifiApSave(nullptr));
  wifiApLoad(); assert(!wifiApPinned());
  wifiApRecoveryLoop(); assert(wifiApRecoveryActive()); // Keep UI available until reconnected.
  WiFi.state = WL_CONNECTED; wifiApRecoveryLoop();
  assert(!wifiApRecoveryActive() && WiFi.apStops == 1);
  WiFi.state = 0; testNow += 1; wifiApRecoveryLoop(); testNow += 60000; wifiApRecoveryLoop();
  assert(!wifiApRecoveryActive()); // Automatic mode retains original recovery behavior.
  testSaved = {1, 2}; wifiApLoad(); assert(!wifiApPinned()); // Malformed storage is ignored.
  puts("WiFi AP persistence, credentials and recovery checks passed");
}
