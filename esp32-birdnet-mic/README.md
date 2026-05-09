<p align="center">
  <img src="../birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi" width="240" />
</p>

# birdnet-esp32-rtsp-mic Firmware

Arduino firmware for an ESP32-C6 I2S microphone that serves **mono 16-bit PCM/L16** audio over
**RTSP** for **BirdNET-Go** and **BirdNET-Pi**. It also provides a Web UI, JSON API, MQTT telemetry,
and Home Assistant MQTT Discovery.

- Latest firmware: **v1.8.0** (2026-05-08)
- Tested board: Seeed Studio **XIAO ESP32-C6**
- Reference microphone: **ICS-43434**; **INMP441** has been reported compatible with the same wiring
- User-facing overview and wiring: `../README.md`
- Changelog: `CHANGELOG.md`
- Web flasher: **https://esp32mic.msmeteo.cz**
- License: MIT (`../LICENSE`)

## Important URLs

Web UI:

```text
http://<device-ip>/
```

RTSP streams:

```text
rtsp://<device-ip>:8554/audio1    Stream 1
rtsp://<device-ip>:8554/audio2    Stream 2
rtsp://<device-ip>:8554/audio     Alias for /audio1, kept for older configs
```

mDNS variants, if enabled and supported by your network:

```text
rtsp://<device-hostname>.local:8554/audio1
rtsp://<device-hostname>.local:8554/audio2
rtsp://<device-hostname>.local:8554/audio
```

The API and Web UI publish `/audio1` and `/audio2`. Use `/audio1` in new configurations. `/audio`
remains available only as a compatibility alias for stream 1.

## First Boot

1. Flash with the web flasher or build manually.
2. The device starts WiFiManager AP **ESP32-RTSP-Mic-AP**.
3. Connect to the AP and open `192.168.4.1` if the captive portal does not open automatically.
4. Save Wi-Fi credentials.
5. After reboot, open `http://<device-ip>/`.

Default hostname is unique per device, for example `esp32mic-a1b2c3`.

## What's New In v1.8.0

- Multi-session RTSP core with configurable **1-3 concurrent sessions**; default is 2.
- Two documented stream paths: `/audio1` and `/audio2`; `/audio` aliases `/audio1`.
- Per-stream enable/disable.
- Per-stream target selection: BirdNET-Go or BirdNET-Pi.
- Transport is selected from target: TCP for BirdNET-Go, UDP for BirdNET-Pi.
- Per-stream live stats: client count, streaming state, packet rate, last play time.
- `/api/status` exposes per-stream URLs, enable state, max clients, and live data.
- Unified Web UI Status card with system info and both stream columns.
- MQTT reconnect is allowed during streaming with a longer retry interval.
- mDNS hostname generation now uses Wi-Fi MAC to avoid collisions on boards from the same vendor.

## Hardware

### I2S Wiring

![Wiring / pinout](../connection.png)

| Mic signal | ESP32-C6 GPIO | Code define |
|---:|:--:|---|
| **BCLK / SCK** | **21** | `I2S_BCLK_PIN` |
| **LRCLK / WS** | **1** | `I2S_LRCLK_PIN` |
| **SD / DOUT** | **2** | `I2S_DOUT_PIN` |
| **VDD** | 3V3 | Power |
| **GND** | GND | Ground |

The firmware configures I2S as master/RX, reads the left channel, then shifts/scales samples to
16-bit PCM. If using INMP441, set `L/R` or `SEL` to the left channel, usually GND.

### XIAO ESP32-C6 Antenna Path

The firmware selects the external antenna path on XIAO ESP32-C6:

```text
GPIO3  -> LOW
GPIO14 -> HIGH
```

Use an external 2.4 GHz antenna for reliable streaming. Weak Wi-Fi increases retries, heat, and the
chance of audio dropouts. If your hardware uses the internal antenna path, adjust the GPIO3/GPIO14
block in `setup()`.

## Runtime Defaults

- Sample rate: 48 kHz
- Audio format: mono 16-bit PCM/L16
- Gain: 1.2
- Buffer: 1024 samples
- I2S shift: 12 bits
- High-pass filter: ON, 500 Hz
- Wi-Fi TX power: about 19.5 dBm
- CPU: 160 MHz
- Thermal shutdown: 80 C, protection ON
- Max RTSP clients: 2
- mDNS: ON
- Time sync: ON

## Web UI

The Web UI runs on port **80** and includes:

- Status: IP, RSSI, uptime, heap, server state, stream states, packet rates.
- Streams: URLs for `/audio1` and `/audio2`, enable/disable, max clients, BirdNET target.
- Audio: sample rate, gain, buffer size, I2S shift, high-pass filter, signal level.
- Time & Network: NTP state, time offset, mDNS, stream schedule, optional deep sleep, Wi-Fi actions.
- Reliability: auto-recovery, threshold mode, check interval, scheduled reset.
- Thermal: current/peak temperature, shutdown limit, protection latch, acknowledgement.
- MQTT & Home Assistant: broker settings, publish interval, discovery republish.
- Logs: ring buffer view and download.
- Actions: RTSP server ON/OFF, reset I2S, reconnect Wi-Fi, reboot, restore defaults.

## Stream Behavior

- Stream 1: `/audio1`; `/audio` is a compatibility alias for the same stream.
- Stream 2: `/audio2`.
- Each stream can be enabled or disabled.
- Each stream can target BirdNET-Go or BirdNET-Pi.
- BirdNET-Go target uses RTP over RTSP/TCP.
- BirdNET-Pi target uses RTP/UDP when the client provides UDP ports.
- RTSP keep-alive via `GET_PARAMETER` is supported.
- Inactive non-streaming sessions time out after about 30 seconds.

### Verify With ffmpeg Tools

```bash
ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
ffprobe -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
```

For stream 2, replace `/audio1` with `/audio2`. If VLC/ffplay works, use the same URL in
BirdNET-Go or BirdNET-Pi.

## Network And Time

- Wi-Fi power save is disabled with `WiFi.setSleep(false)` for stable streaming.
- WiFiManager AP: `ESP32-RTSP-Mic-AP`.
- WiFiManager connect timeout: 60 s.
- WiFiManager portal timeout: 180 s.
- mDNS can be enabled/disabled in the Web UI.
- Hostname can be changed via API: `key=mdns_hostname&value=esp32mic-garden`.
- mDNS often fails on isolated/guest Wi-Fi or inside Docker containers; use device IP in those cases.
- NTP sync runs on boot, retries every hour until synced, then refreshes every 6 hours.
- If time is unavailable, logs fall back to uptime timestamps.

### Stream Schedule And Deep Sleep

Stream schedule is configured in Time & Network.

- Cross-midnight windows are supported, for example `22:00-06:00`.
- If time is invalid, schedule policy is fail-open: streaming stays allowed.
- If start and stop are equal, the window is explicitly empty and streaming is blocked.
- Optional deep sleep can run outside the stream window only when time is valid.
- Deep sleep is blocked during startup grace, with active clients, or without valid time.

API keys:

```text
stream_sched=on|off
stream_start_min=<0..1439>
stream_stop_min=<0..1439>
deep_sleep_sched=on|off
```

## JSON API

The API mirrors the Web UI. Inspect browser DevTools -> Network for exact calls.

Important read endpoints:

```text
GET /api/status
GET /api/audio_status
GET /api/perf_status
GET /api/thermal
GET /api/logs
```

Mutating calls use `POST` and require header `X-ESP32MIC-CSRF: 1`.

Common settings endpoint:

```text
POST /api/set
```

Examples of request bodies:

```text
key=stream1_enabled&value=on
key=stream2_enabled&value=off
key=max_clients&value=2
key=stream1_target&value=0
key=stream2_target&value=1
key=hp_enable&value=on
key=hp_cutoff&value=600
```

Target values:

```text
0 = BirdNET-Go
1 = BirdNET-Pi
```

`/api/status` includes stream URLs and state, including:

```text
stream1_url_ip
stream2_url_ip
stream1_url_mdns
stream2_url_mdns
stream1_enabled
stream2_enabled
stream1_target
stream2_target
s1_clients
s2_clients
s1_streaming
s2_streaming
s1_pkt_rate
s2_pkt_rate
max_clients
```

## MQTT And Home Assistant

MQTT settings are available in the Web UI and API.

- Telemetry topic: `<topic_prefix>/state`
- Availability topic: `<topic_prefix>/availability`
- RTSP server command: `<topic_prefix>/cmd/rtsp_server` with `ON` or `OFF`
- Reboot command: `<topic_prefix>/cmd/reboot` with `PRESS` or `REBOOT`
- Publish interval: default 60 s, range 10-3600 s
- Immediate publishes happen on important events such as stream start/stop and connection changes.

Home Assistant MQTT Discovery creates entities for runtime values such as RSSI, uptime, heap,
temperature, stream state, packet rate, client count, reboot reason, restart counter, firmware
version/build, and Wi-Fi reconnect count.

MQTT password is stored in device flash in plain text NVS.

## Build And Flash Manually

### Arduino IDE

1. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
2. Install an ESP32 Arduino core with ESP32-C6 support.
3. Select *Seeed XIAO ESP32-C6* or *ESP32-C6 Dev Module*.
4. Compile and upload over USB.

### arduino-cli

```bash
arduino-cli compile --fqbn <BOARD_FQBN> esp32-birdnet-mic
arduino-cli upload -p <PORT> --fqbn <BOARD_FQBN> esp32-birdnet-mic
```

### PlatformIO

Typical target is an ESP32-C6 Arduino environment, for example `env:xiao_esp32c6`:

```bash
pio run -t upload
```

## Web UI Development

The Web UI source is `webui/index.html`. Firmware serves the compressed generated header
`WebUI_gz.h` from PROGMEM.

After editing the UI, regenerate the header:

```bash
./tools/gen_webui_gzip_header.sh
```

## Persisted Configuration

Most runtime settings are stored in NVS namespace `audio` through ESP32 Preferences.

Main keys:

```text
sampleRate       Audio sample rate
gainFactor      Audio gain
bufferSize       Samples per packet/buffer profile
shiftBits        I2S right shift before gain
hpEnable         High-pass enable
hpCutoff         High-pass cutoff Hz
wifiTxDbm        Wi-Fi TX power
mdnsEn           mDNS enable
timeSyncEn       NTP enable
timeOffset       Local offset in minutes
strSchedEn       Stream schedule enable
strSchStart      Stream window start minute
strSchStop       Stream window stop minute
deepSchSlp       Deep sleep outside schedule window
autoRecovery     Packet-rate recovery enable
thrAuto          Automatic threshold mode
minRate          Manual minimum packet rate
checkInterval    Recovery check interval in minutes
schedReset       Scheduled reset enable
resetHours       Scheduled reset interval
ohEnable         Thermal protection enable
ohThresh         Thermal shutdown threshold C
ohLatched        Persisted thermal latch
```

Apply changes through Web UI or API. Audio-related updates call `restartI2S()` when needed.

## RTSP Implementation Notes

- `DESCRIBE` returns SDP with `a=rtpmap:96 L16/<sample-rate>/1` and `a=control:track1`.
- Stream selection is path-based: `/audio1` and `/audio` select stream 1; `/audio2` selects stream 2.
- `SETUP` returns either `RTP/AVP/TCP;unicast;interleaved=0-1` or UDP ports, depending on target.
- `PLAY` starts RTP packet output for that session.
- `TEARDOWN` stops the session.
- RTP timestamp increments by the number of audio samples per packet.

## Stability Notes

- Aim for Wi-Fi RSSI better than about **-75 dBm**.
- Increase buffer size in RF-noisy environments; this adds latency but improves stability.
- If audio is noisy while Wi-Fi is otherwise stable, try lowering Wi-Fi TX power before changing audio settings.
- Keep I2S wires short and away from the ESP32 RF area.
- Use shielded cable for longer microphone runs.
- Thermal protection disables RTSP when the chip reaches the configured limit; the latch survives reboot until acknowledged in the Web UI.

## Security

- Keep the device on a trusted LAN.
- Do not expose HTTP, RTSP, or OTA to the public internet.
- Protect OTA with a password if enabled.
- Mutating API endpoints require `POST` and `X-ESP32MIC-CSRF: 1`, but read endpoints are not globally authenticated by default.

## Known Limitations

- No TLS or built-in user authentication for the Web UI/API.
- mDNS depends on multicast support in your LAN and often does not work across VLANs, guest networks, or Docker bridge networks.
- The firmware is primarily tested on Seeed Studio XIAO ESP32-C6 with ICS-43434.

## Credits

- Author: **@Sukecz**

## License

This firmware is released under the MIT License. See `../LICENSE`.
