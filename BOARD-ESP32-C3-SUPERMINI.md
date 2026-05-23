# ESP32-C3 Super Mini + MS3625

This workspace ports [esp32-birdnet-mic](https://github.com/Sukecz/esp32-birdnet-mic) from the upstream **ESP32-C6 (XIAO)** target to the **ESP32-C3 Super Mini** with an **MS3625** I2S microphone (INMP441-compatible).

## Wiring

| MS3625 pin | ESP32-C3 Super Mini | Notes |
|------------|---------------------|--------|
| SCK / BCLK | **GPIO 10** | I2S bit clock |
| WS / LRCLK | **GPIO 4** | I2S word select — GPIO3 is a strapping pin and must not be used |
| SD / DOUT | **GPIO 5** | Mic data in |
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

1. Install the **esp32** board package by Espressif (2.x with ESP32-C3 support).
2. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
3. **Tools → Board:** `ESP32C3 Dev Module`
4. **Flash Size:** 4MB (32Mb)
5. **USB CDC On Boot:** Enabled
6. **Partition Scheme:** Default 4MB with spiffs (or largest app partition if compile is too large)
7. Compile and upload over USB (hold **BOOT** if the port is not detected).

### Web flasher (Chrome / Edge)

No Arduino libraries needed — flash from the browser:

```powershell
cd web-flasher
.\serve.ps1
```

Open **http://localhost:8765**, connect USB, click **Flash firmware**. See [`web-flasher/README.md`](web-flasher/README.md).

After rebuilding firmware, refresh binaries:

```powershell
pio run -e esp32-c3-super-mini
.\scripts\copy-firmware-to-web-flasher.ps1
```

### PlatformIO

From the repo root:

```bash
pio run -e esp32-c3-super-mini
pio run -e esp32-c3-super-mini -t upload
```

A successful release build uses about **1.0 MB** of the **1.28 MB** app partition (78% flash on default 4MB layout).

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
- If Wi-Fi is unstable, lower **Wi-Fi TX Power** in the Web UI (try 8.5–11 dBm). Default for this port is 11 dBm.

## Troubleshooting

- **No audio:** Confirm **L/R → GND**. Adjust **I2S shift** in the Web UI Audio section.
- **I2S errors on boot:** Check wiring against the table above.
- **Wi-Fi won't connect:** Lower TX power; move the board closer to the access point.

## Reverting to XIAO ESP32-C6

Edit `esp32-birdnet-mic/board_profile.h`:

- Set `I2S_BCLK_PIN` 21, `I2S_LRCLK_PIN` 1, `I2S_DOUT_PIN` 2
- Set `BOARD_HAS_XIAO_ANTENNA_SWITCH` to `1`
- Set `BOARD_DEFAULT_WIFI_TX_DBM` to `19.5f`
- Update `BOARD_MODEL_JSON` to `"XIAO ESP32-C6"`

Build for **Seeed XIAO ESP32C6** in Arduino IDE.
