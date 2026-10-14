# ESP32-C3 Super Mini + I2S Microphone

This workspace builds upstream [esp32-birdnet-mic](https://github.com/Sukecz/esp32-birdnet-mic) **v1.26**, commit `85094d7`, for the **ESP32-C3 Super Mini**. The local firmware differences are the board identity, I2S pins, and applying the saved Wi-Fi TX setting before connecting on this board. ICS-43434 and INMP441-compatible microphones use the default Philips I2S format.

## Wiring

| Mic pin | ESP32-C3 Super Mini | Notes |
| --- | --- | --- |
| SCK / BCLK | **GPIO 10** | I2S bit clock |
| WS / LRCLK | **GPIO 4** | I2S word select |
| SD / DOUT | **GPIO 5** | Mic data in |
| L/R or SEL | **GND** | Left channel (required) |
| VDD | **3.3V** | Do not use 5 V |
| GND | **GND** | Common ground |

Keep I2S wires short and away from the onboard ceramic antenna.

Pins are defined in [`esp32-birdnet-mic/board_profile.h`](esp32-birdnet-mic/board_profile.h).

## Build and flash

### Arduino IDE

Arduino IDE does **not** bundle third-party libraries. Install these first via **Sketch → Include Library → Manage Libraries**:

| Library | Author |
| --- | --- |
| **WiFiManager** | tzapu |
| **PubSubClient** | Nick O'Leary |

See also [`esp32-birdnet-mic/arduino-libs.txt`](esp32-birdnet-mic/arduino-libs.txt).

1. Install the **esp32** board package by Espressif **3.3.12**.
2. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
3. **Tools → Board:** `ESP32C3 Dev Module`
4. **Flash Size:** 4MB (32Mb)
5. **USB CDC On Boot:** Enabled
6. **Partition Scheme:** Minimal SPIFFS, with two approximately 1.9 MB OTA slots.
7. Add `-DBIRDNET_C3_SUPER_MINI` to the sketch's `build_opt.h` so the Super Mini pins are selected. PlatformIO already supplies this flag.
8. Compile and upload over USB (hold **BOOT** if the port is not detected).

### PlatformIO

The pinned pioarduino platform supplies Arduino ESP32 3.3.12. It requires Python 3.10-3.14; Python 3.15 is not supported. On the current macOS setup, use the existing Python 3.11 PlatformIO environment:

```bash
~/.platformio/penv/bin/pio run -e esp32-c3-super-mini
~/.platformio/penv/bin/pio run -e esp32-c3-super-mini -t upload
```

Packages are isolated in `~/.platformio-birdnet-mic`. The release build uses approximately **1.34 MB** of the **1.97 MB** app partition (68%). The first upgrade from the old partition layout must use USB, not OTA. NVS stays at the same offset, preserving Wi-Fi credentials and settings; the SPIFFS layout changes.

## First boot

1. Connect to Wi-Fi AP **ESP32-RTSP-Mic-AP**.
2. Open `http://192.168.4.1` and enter your Wi-Fi credentials.
3. Open the Web UI at `http://<device-ip>/`.
4. Test RTSP: `ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio1`

## BirdNET

- **BirdNET-Go:** stream target **BirdNET-Go**, URL `rtsp://<device-ip>:8554/audio1` (TCP).
- **BirdNET-Pi:** stream target **BirdNET-Pi** if you use UDP RTP.

## Important limitations

- Do **not** use upstream XIAO binaries on the Super Mini: their microphone pins differ. Official automatic OTA is disabled for this board; manual OTA remains available for locally built application binaries after the USB partition upgrade.
- Build and flash your own firmware from this repo.
- Network behavior follows upstream: **15 dBm** default TX power and Wi-Fi sleep disabled. User-selected power is saved by the Web UI; the Super Mini applies it before association as well as through the normal upstream post-connection path. No power value is forced by the board adaptation.
- Audio defaults follow upstream: **48 kHz**, **512 samples**, gain **1.5**, and a **64 KiB** internal audio reserve. Existing audio preferences survive the upgrade and can be changed through the Web UI.
- Monitor physical board temperature during streaming, especially when increasing TX power. Thermal suitability on the Super Mini has not been established.

## Troubleshooting

- **No audio:** Confirm **L/R → GND**. Adjust **I2S shift** in the Web UI Audio section.
- **I2S errors on boot:** Check wiring against the table above.
- **Wi-Fi won't connect:** Check saved credentials and access-point selection in the recovery Web UI.

## Reverting to XIAO ESP32-C6

Build for **Seeed XIAO ESP32C6** in Arduino IDE without `BIRDNET_C3_SUPER_MINI`. Upstream then selects the XIAO pins, RF switch and OTA artifact automatically; the Super Mini profile does not need editing.
