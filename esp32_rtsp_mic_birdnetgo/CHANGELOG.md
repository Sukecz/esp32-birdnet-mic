# Changelog

## 1.4.0 — 2026-11-25
- Audio pipeline: the microphone is now read by a dedicated FreeRTOS task into a ~680 ms ring buffer. Slow Wi-Fi writes or Web UI requests can no longer stall I2S and silently drop audio (heard as clicks, skips or "time jumps").
- RTSP/RTP: each packet is sent as one contiguous write (was 3 small TCP writes). Short Wi-Fi stalls are retried for up to 2 s; if a write still cannot finish, the connection is closed cleanly so the client reconnects at once instead of hanging on a half-sent packet.
- Settings changes / auto-recovery that restart I2S now close the active stream so the client reconnects right away with the new parameters (previously the stream went silent until the client timed out).
- New Web UI card "Stream Diagnostics" + API fields: mic overruns, buffer drops, buffer peak fill, send stalls.
- New test-tone mode (`test_tone`, `test_tone_hz`): streams a clean −12 dBFS sine instead of the mic to tell firmware/network problems apart from mic/wiring problems. Not persisted; OFF after reboot.
- HPF coefficient updates are now thread-safe with the capture task.
- Ported I2S capture to the new ESP-IDF driver (`driver/i2s_std.h`); builds on Arduino-ESP32 3.3.x and 4.x (the legacy `driver/i2s.h` no longer exists in 4.x). Same on-wire format: Philips I2S, 32-bit, mono left.
- Note: on core 4.x the binary exceeds the default 1.25 MB app slot; use the "No OTA (2MB APP)" partition scheme or stay on core 3.3.x.

## 1.3.0 — 2025-09-09
- Thermal protection: added configurable shutdown limit (30–95 °C, default 80 °C) with protection enabled by default.
- Thermal latch now persists across reboots and must be acknowledged in the Web UI before RTSP can be re-enabled; UI includes clear button and richer status strings.
- Firmware: on overheat the RTSP server is stopped, the reason/temperature/timestamp are persisted, and a manual restart is required.
- Web UI: Thermal card now exposes the protection toggle, limit selector, status badge, last shutdown log, and detailed EN/CZ tooltips.
- Docs: refreshed defaults and added guidance for the new thermal workflow.

## 1.2.0 — 2025-09-08
- Added configurable High‑pass filter (HPF) to reduce low‑frequency rumble
- Web UI: Signal level meter with clip warning and beginner guidance (EN/CZ)
- RTSP: respond to `GET_PARAMETER` (keep‑alive) for better client compatibility
- API: `/api/status` now includes `fw_version`
- Docs: README updated (defaults, HPF notes, RTSP keep‑alive)
- Cleanup: removed unused arpa/inet dependency from source
- Defaults: Gain 1.2, HPF ON at 500 Hz

## 1.1.0 — 2025-09-05
- Web UI redesign: responsive grid, dark theme, cleaner cards
- Simplified controls: removed client Start/Stop/Disconnect; Server ON/OFF only
- Inline editing: change Sample Rate, Gain, Buffer, TX Power directly in fields
- Reliability: Auto/Manual threshold mode with auto‑computed min packet‑rate
- New settings: Scheduled reset (ON/OFF + hours), CPU frequency (MHz)
- Logs: larger panel; every UI action and setting change is logged
- Performance: faster initial load; immediate apply on Enter/blur
- Thermal: removed periodic temperature logging (kept high‑temp warning)

## 1.0.0 (Initial public release)
- Web UI on port 80 (English/Czech)
- JSON API endpoints (status, audio, performance, thermal, logs, actions, settings)
- In-memory log buffer, performance diagnostics, auto-recovery
- OTA and WiFiManager included
