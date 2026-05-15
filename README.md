<p align="center">
  <img src="birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi" width="260" />
</p>

# birdnet-esp32-rtsp-mic

ESP32-C6 network microphone for **BirdNET-Go** and **BirdNET-Pi**. It reads an I2S MEMS microphone
and serves mono **16-bit PCM/L16** audio over **RTSP**.

- Latest firmware: **v1.9.1** (2026-05-15)
- Target sketch: `esp32-birdnet-mic`
- Web flasher: **https://esp32mic.msmeteo.cz** (Chrome/Edge desktop, USB-C data cable)
- Detailed firmware docs: `esp32-birdnet-mic/README.md`
- Changelog: `esp32-birdnet-mic/CHANGELOG.md`
- License: MIT (`LICENSE`)

## Quick Start

1. Open **https://esp32mic.msmeteo.cz**.
2. Click **Flash**, select the USB JTAG/serial device, and wait for reboot.
3. On first boot, connect to Wi-Fi AP **ESP32-RTSP-Mic-AP**.
4. Open `http://192.168.4.1` if the captive portal does not appear, then enter your Wi-Fi details.
5. After reboot, open the device Web UI at `http://<device-ip>/`.
6. Use these RTSP URLs in BirdNET-Go, BirdNET-Pi, VLC, or ffplay:

```text
rtsp://<device-ip>:8554/audio1    Stream 1
rtsp://<device-ip>:8554/audio2    Stream 2
rtsp://<device-ip>:8554/audio     Alias for /audio1, kept for compatibility
```

If mDNS is enabled and works on your LAN, the same paths are available through the device hostname:

```text
rtsp://<device-hostname>.local:8554/audio1
rtsp://<device-hostname>.local:8554/audio2
rtsp://<device-hostname>.local:8554/audio
```

The default hostname is unique per device, for example `esp32mic-a1b2c3`.

## Wiring

Tested hardware: **Seeed Studio XIAO ESP32-C6** + **ICS-43434** I2S microphone.

![Wiring / pinout](connection.png)

| Mic signal | XIAO ESP32-C6 GPIO | Notes |
|---|:--:|---|
| **BCLK / SCK** | **21** | I2S bit clock |
| **LRCLK / WS** | **1** | I2S word select |
| **SD / DOUT** | **2** | I2S data from microphone |
| **VDD** | 3V3 | Power |
| **GND** | GND | Ground |

**INMP441** has also been reported to work with the same I2S pins. If your board exposes `L/R` or
`SEL`, set it to the left channel, usually GND, because the firmware reads the left I2S channel.
See discussion #25: https://github.com/Sukecz/esp32-birdnet-mic/discussions/25

## XIAO ESP32-C6 Antenna

XIAO ESP32-C6 uses an RF switch path controlled by GPIO3/GPIO14. The firmware selects the external
antenna path by default.

- Use a 2.4 GHz external antenna for stable RTSP streaming.
- Weak Wi-Fi can cause packet loss, retries, and extra heat.
- If your hardware intentionally uses the internal antenna path, adjust or remove the GPIO3/GPIO14
  antenna-control block in `setup()`.

## Test The Stream

Use `/audio1` for stream 1 unless you specifically need the old `/audio` URL.

```bash
ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
ffprobe -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
```

For VLC: *Media* -> *Open Network Stream* -> paste `rtsp://<device-ip>:8554/audio1` or
`rtsp://<device-ip>:8554/audio2`.

If VLC/ffplay works, use the same RTSP URL in BirdNET-Go or BirdNET-Pi.

## What The Firmware Provides

- English Web UI on port **80** with live status, settings, logs, and actions.
- JSON API for automation and monitoring.
- Two RTSP streams: `/audio1` and `/audio2`; `/audio` is an alias for `/audio1`.
- Configurable **1-3 concurrent RTSP sessions**; default is 2.
- Per-stream enable/disable and BirdNET-Go / BirdNET-Pi target selection.
- Transport selection based on target: TCP for BirdNET-Go, UDP for BirdNET-Pi.
- Dedicated audio producer task with ring buffer between I2S capture and RTSP packet output.
- Audio/API diagnostics for I2S errors, ring-buffer drops, and RTSP write stalls/timeouts.
- MQTT telemetry and Home Assistant MQTT Discovery.
- Stream schedule by local time, including overnight windows.
- Optional deep sleep outside the stream schedule window.
- Auto-recovery, scheduled reset, CPU frequency control.
- Thermal protection with persistent latch and manual acknowledgement.
- Configurable high-pass filter for low-frequency rumble.
- mDNS hostname support and OTA support.

## Recommended Hardware

| Part | Qty | Notes | Link |
|---|---:|---|---|
| Seeed Studio XIAO ESP32-C6 | 1 | Tested target board | [AliExpress](https://www.aliexpress.com/item/1005007341738903.html) |
| MEMS I2S microphone **ICS-43434** | 1 | Tested reference microphone | [AliExpress](https://www.aliexpress.com/item/1005008956861273.html) |
| MEMS I2S microphone **INMP441** | 1 | Reported compatible with same wiring | - |
| Shielded cable, 6 core | Optional | Helps on longer microphone runs | [AliExpress](https://www.aliexpress.com/item/1005002586286399.html) |
| 5 V power supply | 1 | At least 1 A recommended | [AliExpress](https://www.aliexpress.com/item/1005002624537795.html) |
| 2.4 GHz antenna, IPEX/U.FL | Recommended | Important for XIAO ESP32-C6 stability | [AliExpress](https://www.aliexpress.com/item/1005008490414283.html) |

Links are examples only. Verify the exact part number before buying.

## Practical Tips

- Keep I2S wires short; use shielded cable for longer runs.
- Keep the microphone and I2S wires away from the ESP32 antenna/RF area.
- Aim for Wi-Fi RSSI better than about **-75 dBm**.
- Use DHCP reservation or fixed IP if BirdNET cannot resolve mDNS reliably.
- Keep the device on a trusted LAN; do not expose HTTP/RTSP to the public internet.

### RF Noise / Wi-Fi TX Power

If the stream is stable but the audio is noisy or distorted, try lowering **Wi-Fi TX Power** in the
Web UI. When the access point is nearby, lower TX power can reduce RF coupling into the microphone,
wiring, or power rails.

Suggested test:

1. Set Wi-Fi TX Power to about **11 dBm**.
2. Watch RSSI, packet rate, reconnects, and audio quality for a few minutes.
3. If Wi-Fi becomes unstable, raise TX power step by step.

A grounded metal enclosure can help shield microphone wiring, but keep the Wi-Fi antenna outside the
metal enclosure.

### High-Pass Filter

The firmware includes a configurable high-pass filter to reduce low-frequency rumble.

- Default: ON at 500 Hz.
- Typical range: 300-800 Hz.
- Web UI: Audio -> `High-pass` and `HPF Cutoff`.
- API: `POST /api/set` with `key=hp_enable&value=on|off` or `key=hp_cutoff&value=600`.
- Mutating API calls require header `X-ESP32MIC-CSRF: 1`.

## Compatibility

- Target board: ESP32-C6, tested with Seeed Studio XIAO ESP32-C6.
- Reference microphone: ICS-43434.
- Reported compatible microphone: INMP441 with the same I2S wiring.
- Other ESP32 boards or I2S microphones may work, but may need pin or I2S format changes.

## Arduino IDE Build Size

Firmware v1.9.1 includes `esp32-birdnet-mic/build_opt.h`, which the ESP32 Arduino core loads
automatically in Arduino IDE and `arduino-cli`. It disables unused C++ exception/unwind metadata and
keeps the default XIAO ESP32-C6 partition scheme below the 1.2 MB app limit with about 60 KB reserve,
without removing firmware features.

## More Documentation

- Firmware details, build instructions, API notes: `esp32-birdnet-mic/README.md`
- Web flasher notes: `web-flasher/README.md`
- Firmware changes: `esp32-birdnet-mic/CHANGELOG.md`

## License

This project is released under the MIT License. See `LICENSE`.
