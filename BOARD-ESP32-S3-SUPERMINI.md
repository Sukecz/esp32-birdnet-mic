# ESP32-S3 Super Mini + MS3625

This workspace ports [esp32-birdnet-mic](https://github.com/Sukecz/esp32-birdnet-mic) from the upstream **ESP32-C6 (XIAO)** target to the **ESP32-S3 Super Mini** with an **MS3625** I2S microphone (INMP441-compatible).

## Wiring

| MS3625 pin | ESP32-S3 Super Mini | Notes |
|------------|---------------------|--------|
| SCK / BCLK | **GPIO 4** | I2S bit clock |
| WS / LRCLK | **GPIO 5** | I2S word select |
| SD / DOUT | **GPIO 6** | Mic data in |
| L/R or SEL | **GND** | Left channel (required) |
| VDD | **3.3V** | Do not use 5 V |
| GND | **GND** | Common ground |

Keep I2S wires short and away from the onboard ceramic antenna.

Pins are defined in [`esp32-birdnet-mic/board_profile.h`](esp32-birdnet-mic/board_profile.h).

## Build and flash

### Arduino IDE

Arduino IDE does **not** bundle third-party libraries. Install these first via **Sketch → Include Library → Manage Libraries**:

| Library | Author (search name) |
|---------|----------------------|
| **WiFiManager** | tzapu |
| **PubSubClient** | Nick O'Leary |

See also [`esp32-birdnet-mic/arduino-libs.txt`](esp32-birdnet-mic/arduino-libs.txt).

1. Install the **esp32** board package by Espressif (3.x with ESP32-S3 support).
2. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
3. **Tools → Board:** `ESP32S3 Dev Module`
4. **Flash Size:** 4MB (32Mb)
5. **USB CDC On Boot:** Enabled
6. **USB Mode:** Hardware CDC and JTAG
7. **Partition Scheme:** Default 4MB with spiffs
8. Compile and upload over USB (hold **BOOT** if the port is not detected).

### PlatformIO

From the repo root, switch `board_profile.h` to the S3 configuration (see below), then:

```bash
pio run -e esp32-s3-super-mini
pio run -e esp32-s3-super-mini -t upload
```

A successful release build uses about **1.0 MB** of the **1.28 MB** app partition (78% flash on default 4MB layout).

## Switching board_profile.h for S3

Edit `esp32-birdnet-mic/board_profile.h` to:

```cpp
#define BOARD_NAME "ESP32-S3 Super Mini"
#define BOARD_MODEL_JSON "ESP32-S3 Super Mini"
#define I2S_BCLK_PIN  4
#define I2S_LRCLK_PIN 5
#define I2S_DOUT_PIN  6
#define BOARD_HAS_XIAO_ANTENNA_SWITCH 0
#define BOARD_DEFAULT_WIFI_TX_DBM 19.5f
#define I2S_DMA_USE_BUF_FIELDS 1
```

And add a `[env:esp32-s3-super-mini]` entry in `platformio.ini`:

```ini
[env:esp32-s3-super-mini]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.flash_size = 4MB
board_build.flash_mode = dio
board_build.partitions = default.csv
board_build.f_cpu = 240000000L
board_upload.flash_size = 4MB
board_upload.flash_mode = dio
lib_deps =
    tzapu/WiFiManager@^2.0.17
    knolleary/PubSubClient@^2.8
build_flags =
    -fno-exceptions
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
```

## First boot

1. Connect to Wi-Fi AP **ESP32-RTSP-Mic-AP**.
2. Open `http://192.168.4.1` and enter your Wi-Fi credentials.
3. Open the Web UI at `http://<device-ip>/`.
4. Test RTSP: `ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio1`

## BirdNET

- **BirdNET-Go:** stream target **BirdNET-Go**, URL `rtsp://<device-ip>:8554/audio1` (TCP).
- **BirdNET-Pi:** stream target **BirdNET-Pi** if you use UDP RTP.

## Important limitations

- Do **not** use the upstream [web flasher](https://esp32mic.msmeteo.cz) or `manual-ota-firmware/firmware-app.bin` — those binaries are built for **ESP32-C6 only**.
- Build and flash your own firmware from this repo.
- The `esp32-s3-devkitc-1` board definition defaults to 8 MB flash. The S3 Super Mini has **4 MB**. The `platformio.ini` configuration above explicitly overrides this with both `board_build.flash_size` and `board_upload.flash_size`.

## Troubleshooting

- **No audio:** Confirm **L/R → GND**. Adjust **I2S shift** in the Web UI Audio section.
- **I2S errors on boot:** Check wiring against the table above.
- **Bootloop / device keeps connecting and disconnecting:** The board is in a bootloop, usually caused by flash size mismatch. Make sure both `board_build.flash_size = 4MB` and `board_upload.flash_size = 4MB` are set in `platformio.ini`.
- **Wi-Fi won't connect:** Move the board closer to the access point.
