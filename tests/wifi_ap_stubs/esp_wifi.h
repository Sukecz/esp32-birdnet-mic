#pragma once
#include <cstdint>
struct wifi_config_t { struct { uint8_t ssid[32]; uint8_t password[64]; } sta; };
constexpr int WIFI_IF_STA = 0, ESP_OK = 0;
extern wifi_config_t testConfig;
inline int esp_wifi_get_config(int, wifi_config_t *out) { *out = testConfig; return ESP_OK; }
