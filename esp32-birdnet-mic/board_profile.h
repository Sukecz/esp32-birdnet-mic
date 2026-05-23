#pragma once

#define BOARD_NAME "ESP32-C3 Super Mini"
#define BOARD_MODEL_JSON "ESP32-C3 Super Mini"

// I2S mic wiring: connect BCLK->GPIO10, WS->GPIO4, DATA->GPIO5
// NOTE: GPIO3 is a strapping/JTAG pin on C3 and must NOT be used for I2S WS
#define I2S_BCLK_PIN  10
#define I2S_LRCLK_PIN 4
#define I2S_DOUT_PIN  5

// No RF switch on C3 Super Mini
#define BOARD_HAS_XIAO_ANTENNA_SWITCH 0

// C3 Super Mini is sensitive to high TX power; keep at 11 dBm
#define BOARD_DEFAULT_WIFI_TX_DBM 11.0f

// Arduino-ESP32 3.x legacy I2S driver uses dma_buf_* field names on C3
#define I2S_DMA_USE_BUF_FIELDS 1
