<p align="center">
  <img src="../assets/birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi" width="240" />
</p>

# BirdNETMic Firmware — Streaming and standalone Recording

Two modes: live **RTSP Streaming** or standalone **microSD WAV Recording**.
Start with the [beginner guide](../README.md) and [field recording guide](../docs/field-recording.md).
Current firmware: **2.03** ([validation record](../release-notes/2.03-validation.md)).
For new builds we recommend **XIAO ESP32-C5 + IM73D122 PDM**, with an Adafruit BFF for SD.

Arduino firmware for Seeed Studio XIAO ESP32 and M5Stack AtomS3U, using I2S or PDM microphones to serve **mono 16-bit PCM/L16** audio over
**RTSP** for **BirdNET-Go** and **BirdNET-Pi**. It also provides a Web UI, JSON API, MQTT telemetry,
and Home Assistant MQTT Discovery.

- Current firmware: **v2.03** — Recording memory fixes, Listen recovery and custom device Wi-Fi passwords.
- [Overview, support status and parts](../README.md) · [Release history](CHANGELOG.md)
- Build targets: Seeed Studio **XIAO ESP32-C3**, **XIAO ESP32-S3**, **XIAO ESP32-C5**, **XIAO ESP32-C6**
- Alternative I2S microphone: **Adafruit SPH0645LM4H** in selectable MSB / left-justified mode
- Legacy microphones: **ICS-43434/INMP441** in default Philips I2S mode
- User-facing overview and wiring: `../README.md`
- Changelog: `CHANGELOG.md`
- Web flasher: **https://esp32mic.msmeteo.cz**
- Manual OTA firmware: `../manual-ota-firmware/firmware-app-<board>.bin` (`firmware-app.bin` remains the C6 alias)
- License: MIT (`../LICENSE`)

## Current firmware overview

- Two RTSP streams, browser listening, gain/HPF and recommended microphone settings.
- Local Web UI with save/discard feedback, JSON API, logs and microphone/stream diagnostics.
- MQTT, Home Assistant discovery, local-time schedules and optional deep sleep.
- Hardware PDM conversion on S3; software PDM conversion on C3/C5/C6; mono 48 kHz output.
- S3/C5 PSRAM audio buffers with an internal-memory fallback; optional 240 MHz on S3/C5.
- Capture pauses during OTA and thermal shutdown; optional recovery after cooling and
  immediate manual acknowledgement when thermal protection is explicitly OFF.
- Board-specific USB installation and OTA updates that preserve saved settings and partitions.

See the [changelog](CHANGELOG.md) for the version-by-version history and
the [main guide](../README.md) for hardware choices, quick start and wiring.

## S3 board selection: AtomS3U and XIAO ESP32-S3 Sense

[M5Stack AtomS3U](https://docs.m5stack.com/en/core/AtomS3U) and
[XIAO ESP32-S3 Sense](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
use the shared **S3 firmware**. **AtomS3U** has a built-in PDM microphone. **Sense remains experimental.**
Open **Device configuration → Board** in the device Web UI,
select the board, and save the configuration. Its built-in PDM microphone runs at
48 kHz; the selection is saved across restarts and OTA updates. Adjust Gain using
the signal meter and Listen; **Gain 24** is a starting point for AtomS3U.

The **XIAO ESP32-S3 / Plus (default)** option keeps the usual wiring
and microphone-format selection. The selector is shown only in S3 firmware.
The Sense profile also accepts external I2S microphones using the normal S3 wiring.
PDM in that profile uses the built-in microphone. For an external PDM microphone,
use the default board option.

In Recording mode, selecting **Sense** also selects its
built-in microSD slot: **CS GPIO21, SCK GPIO7, MISO GPIO8, MOSI GPIO9**. Use the
Sense expansion board with its factory SD connection enabled and a **FAT32 card
up to 32 GB**. Other XIAO profiles retain external SPI SD on D6/D8/D9/D10;
AtomS3U has no supported SD interface. Switching the S3 board while idle in
Recording rechecks the selected slot. Active recording blocks a board change.
See [Seeed's SD guide](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
and the [field recording guide](../docs/field-recording.md).
See the main guide for setup.

## Battery

The main UI's **Battery** section provides **Enable battery monitoring**, wiring
information and voltage calibration. Save the monitoring switch to show
**Battery: ~65% · 3.85 V** in the top overview in both modes. Device configuration
uses the same saved switch; monitoring is off by default.

Use a protected **4.2 V maximum Li-ion/LiPo 1S pack**, including matched parallel
cells. The updated [connection diagram and recommendations](../README.md#battery)
show a **1S BMS**, a fuse in the positive output lead and the external divider
on C6. The illustrated assembly uses a **3 A BMS / 3.5 A fuse**; component values
must be checked against the actual pack, wiring and protection specifications.
All device connections and divider returns belong on the protected **P+/P−**
side, with XIAO BAT+ fed through the fuse.

C5 has a built-in **100 kΩ + 100 kΩ** divider (GPIO6, enable GPIO26); no external
measurement resistors are needed. C3/S3/C6 use an external **100 kΩ + 100 kΩ**
divider to **D0/A0**, leaving microphone and SD pins available.
**M5Stack AtomS3U battery monitoring is unsupported.**

Enter a simultaneous multimeter reading across XIAO BAT+/BAT− and press
**Calibrate** to save a voltage correction within **±10%**. The corrected voltage
also drives the estimated percentage. **Reset calibration** restores 0%.
Calibration survives restart/OTA and is cleared by factory reset.

Voltage-only percentages are approximate; battery/USB/charging detection is
not automatic. Optional **Low-battery protection** stops and saves Recording,
then stays awake or enters deep sleep. Stop voltage, confirmation, recovery
margin and the action are configurable. **Turn Wi-Fi off until restart** in
Recording keeps recording and monitoring active; its delay is configurable.
Reads are staged in the app loop and reported in `/api/status`, independently
of the browser. See the
[battery guide](../docs/battery-monitoring.md) for GPIOs, APIs and validation limits.

## Audio memory by board (v2.03)

The build helper enables S3's 8 MB OPI PSRAM (`PSRAM=opi`) and C5's 8 MB
PSRAM (`PSRAM=enabled`). Streaming puts its normally 64 KiB RTSP reserve there;
browser listening uses a separate 32 KiB PSRAM queue with a 4 KiB internal
fallback. Disabled/unavailable PSRAM and allocation failures retain the adaptive
internal Streaming reserve. C3/C6 use internal RAM and the existing 16 KiB Listen
queue. DMA, PDM filter and capture/RTP working buffers always remain internal.

Recording allocates no RTSP reserve on any board. Its independent SD PCM queue
uses 512 or 128 KiB of PSRAM when available, otherwise an adaptive 32–128 KiB
internal queue retaining 72 KiB of internal headroom. The queue is allocated only
when recording starts and released before returning to Streaming. The SD worker
is retained for later checks. A card check or log read allocates no PCM queue and
uses a separate 48 KiB internal reserve plus the first worker's 8 KiB budget.
PSRAM recording queues use that same SD working reserve, with actual heap checked
again after allocations. Recording capture restarts budget their producer stack
separately from the 48 KiB working reserve; Streaming limits remain unchanged.
Capture and Listen remain active in Recording; only the unused RTSP buffer is
removed. Mode changes stop the producer before changing buffer ownership and
restore the previous mode if capture cannot restart.

`/api/audio_status` reports `rb_in_psram`, `rb_capacity_bytes` (zero in Recording)
and `listen_queue_bytes`. All boards expose `internal_heap_free_kb`,
`internal_heap_largest_kb`, `internal_heap_min_kb`, `psram_total_kb` and
`psram_free_kb` in `/api/status`. These diagnostic fields use KiB despite the
legacy `_kb` suffix. The true minimum captures brief dips missed by UI sampling.

Listen has its own transport task with a 3 KiB internal stack and an independent
PCM queue, so synchronous web requests cannot starve its sender. Each pass is
bounded to eight nonblocking writes and 2 ms. Temporary preview overflows discard
old preview PCM and resume with current audio; partial TCP blocks are retained to
preserve sample alignment. Temporary TCP stalls can recover for up to five seconds;
the sender tries the socket before timing out. An empty capture queue has a separate
two-second starvation limit. Capture
and SD recording never wait for a browser. This live preview can skip delayed
sound; the independent SD WAV path retains its separate loss/error accounting. `/api/audio_status` reports `listen_last_stop`,
`listen_overflows`, `listen_send_timeouts`, `listen_socket_errors`,
`listen_max_loop_gap_ms`, `listen_bytes_sent`, `listen_preview_discarded_bytes`,
`listen_independent_worker`, `listen_worker_stack_free_bytes`,
`listen_max_send_stall_ms`, `listen_send_recoveries` and
`listen_no_audio_timeouts` for diagnosis.

## Important URLs

Firmware 2.00 confirms time synchronization from the SNTP callback,
tracks the first Wi-Fi connection and retries unanswered NTP requests after
30 seconds. `/api/status` separates `time_valid` from `time_synced`; valid clock
values remain visible while synchronization is pending. Post-OTA checks now
include NTP, UTC accuracy and local offset, in addition to microphone capture:
[time-sync validation](../docs/time-sync-validation.md).

Web UI:

```text
http://<device-ip>/
```

RTSP streams:

```text
rtsp://<device-ip>:8554/audio1    Stream 1
rtsp://<device-ip>:8554/audio2    Stream 2
```

mDNS variants, if enabled and supported by your network:

```text
rtsp://<device-hostname>.local:8554/audio1
rtsp://<device-hostname>.local:8554/audio2
```

The API and Web UI publish `/audio1` and `/audio2`. Use `/audio1` in new configurations. `/audio`
remains available only as a compatibility alias for stream 1.

## Access point selection

In **Network → Access point**, leave **Default connection** enabled for the usual
Wi-Fi behavior, or choose **Choose a specific access point**. Click **Find access points**, select
one of the access points for your saved Wi-Fi network, then **Save and connect**. Results are
ordered by signal strength (for example, -58 dBm is stronger than -75 dBm). The current connection
can also be selected without scanning. Scanning may briefly affect streaming; changing the
selection disconnects existing streams while Wi-Fi reconnects.

The selected BSSID is saved across reconnects, restarts and OTA updates. The device does not
fall back to another access point while locked. In the current firmware, after
15 seconds offline the device creates **BirdNETMic-XXXXXX**. Use the password from
**Network → Direct Wi-Fi access**, then open **http://192.168.4.1** to change the selection.
Recovery Wi-Fi stays available while a client is connected. With no clients, it closes after
home Wi-Fi has been connected for at least 60 seconds. Standalone Recording keeps its AP.
Reset Wi-Fi and factory reset also clear the saved AP selection.

API: `GET /api/wifi/ap` returns the saved/current AP and connection status.
`POST /api/wifi/ap` saves `bssid=AA:BB:CC:DD:EE:FF`; an empty `bssid=` restores automatic selection.
`POST /api/wifi/scan` starts one asynchronous scan; poll `GET /api/wifi/scan` for same-SSID results.
POST requests require the existing `X-ESP32MIC-CSRF: 1` header. Saving reconnects Wi-Fi.

## First Boot

1. Flash with the web flasher or build manually.
2. Firmware 2.00 starts the open setup AP **BirdNETMic-XXXXXX**.
3. Connect to the AP and open `192.168.4.1` if the captive portal does not open automatically.
4. Choose the board variant, default startup mode and connection. C6 also offers antenna selection.
   Streaming uses home Wi-Fi; Recording can use home Wi-Fi or standalone device Wi-Fi.
   Save the randomly generated eight-character device Wi-Fi password shown on the page.
5. Finish setup and open the controls. The device AP then requires the displayed password.
   Microphone, SD and recording options are configured in the main UI.

Existing configured devices retain their credentials and settings and skip the welcome page.
After updating, save the recovery password from **Network → Direct Wi-Fi access**.
**Device configuration** changes the default startup mode and connection later; the main
mode switch changes only the current mode. Recording auto-start remains a separate option.
An unavailable home network automatically enables the control AP in both modes without
erasing credentials or rebooting. Recording does not depend on a network connection.
Older published firmware still uses the original WiFiManager portal.

Setup API: `GET /api/setup` reports public setup/connection state without passwords.
Protected POST actions use `X-ESP32MIC-CSRF: 1`: `/api/setup/scan` starts an asynchronous
all-network scan (poll its GET endpoint); `/api/setup/connect` tests `ssid` and `password`
without saving them; `/api/setup` saves `mode`, `connection=home|direct`, S3 `variant`,
and C6 `antenna=external|internal`. `allow_offline=1` finishes without requiring a connection.
`POST /api/setup/access` returns the device AP password for display to the user.
New setup uses eight readable characters. Existing configured passwords are retained;
`shorten=1` explicitly replaces a longer password while no AP clients or recording are active.
Only successfully tested new credentials are committed when setup is saved.

Default hostname is unique per device, for example `esp32mic-a1b2c3`.

## Hardware

### I2S Wiring

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/connection-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="../assets/connection.png">
  <img alt="Wiring diagram for a XIAO ESP32-C6 and I2S microphone; ICS-43434 module shown" src="../assets/connection.png">
</picture>

*ICS-43434 module shown; use the same connections for SPH0645LM4H and INMP441.*

Use the same physical XIAO pin labels on every supported board. The underlying GPIO numbers differ
by chip.

| Mic signal | XIAO pin label | C3 GPIO | S3 GPIO | C5 GPIO | C6 GPIO | Code define |
|---:|:--:|:--:|:--:|:--:|:--:|---|
| **BCLK / SCK** | **D3** | 5 | 4 | 7 | 21 | `I2S_BCLK_PIN` |
| **LRCLK / WS** | **D1** | 3 | 2 | 0 | 1 | `I2S_LRCLK_PIN` |
| **SD / DOUT** | **D2** | 4 | 3 | 25 | 2 | `I2S_DOUT_PIN` |
| **MCLK / SCKI** | **D7** | 20 | 44 | 12 | 17 | `I2S_MCLK_PIN` |
| **VDD** | 3V3 | - | - | - | - | Power |
| **GND** | GND | - | - | - | - | Ground |

Use the same physical wiring for the I2S microphones listed here and power them from **3.3 V**. Connect
`L/R` or `SEL` to **GND** because the firmware reads the left channel. Then select the matching
format:

- **Adafruit SPH0645LM4H (I2S alternative):** MSB / left-justified; select it in
  **Audio -> Microphone format** or set
  API key `mic_format` to `1`.
- **ICS-43434 / INMP441 (legacy):** standard Philips I2S; firmware default.

SPH0645 is supported. ICS-43434 and INMP441 remain supported for
existing builds.

The wrong format only produces incorrectly decoded audio; it does not change the wiring or damage
the microphone. Incorrect wiring or 5 V power can still cause damage.

Firmware v1.21 outputs MCLK at 256 times the configured sample rate. This allows experimental
PCM1808 ADC hardware to run in slave I2S mode with its left input captured by the existing mono
pipeline. **Testing has not yet produced clean audio without noise**, so PCM1808 remains
experimental and is not recommended for a new build. No purchase link is provided. Signal level,
I2S Shift, analog gain and noise performance need further investigation on the complete setup.

<a id="pdm-microphones-experimental"></a>

### PDM microphones

![XIAO ESP32-C6 wiring diagram with an IM73D122 PDM microphone](../assets/connection-IM73D122.png)

*IM73D122 breakout shown: CLK → D3, DATA → D2, L/R → GND; leave NC disconnected.
Power the microphone from 3V3 and the XIAO from USB-C or a regulated 5 V supply.*

**Supported.** See the [microphone comparison](../README.md#microphones)
for the supported I2S options and the **Best quality** IM73D122 PDM option.

PDM choices include **IM73D122**, **Adafruit PDM MEMS Microphone Breakout #3492 (MP34DT01-M)**
and generic **MP34DT01 PDM modules**. Select PDM mode for these modules. These are different chips using the same PDM mode,
not alternative names for IM73D122. Other PDM models require checking their supply rating,
3.072 MHz clock support, left-slot output and actual hardware operation.

Choose **PDM microphone (IM73D122)** under **Audio → Microphone format** after connecting
an external PDM microphone. PDM captures mono 48 kHz audio and keeps the existing RTSP/L16
output, gain, high-pass filter and diagnostics. I2S Shift is ignored. The firmware selects
hardware conversion on XIAO ESP32-S3 and software conversion on XIAO ESP32-C3/C5/C6.
Philips I2S remains the default; existing saved settings are retained on update.

Switch power off before rewiring. For a compatible PDM breakout suitable for 3.3 V:

| Microphone | XIAO connection |
| --- | --- |
| VDD / VCC / 3V | 3V3 (verify the breakout supply specification) |
| GND | GND |
| CLK | D3 |
| DATA / DAT | D2 |
| SEL / L-R | GND (left slot) |

Leave D1 (WS) and D7 (MCLK) disconnected. The PDM clock is 3.072 MHz. Different breakout
pin orders require checking their labels; the mechanical adapter is optional. Changing
format briefly interrupts RTSP. If setup fails, format, rate and related settings roll back.
Selecting PDM sets 48 kHz; switching back to I2S retains that rate, which can then be changed.

Software conversion uses the v15.6 integer filter as its starting point, with streaming
framing and capture integration maintained here. Its useful passband is approximately 0–16 kHz
at 48 kHz output; this is not a claim of flat response across the entire Nyquist band.
The API reports `pdm_supported`, `pdm_decoder`, `pdm_clock_hz`, `pdm_word_stride` and
`pdm_bit_order`. A raw stride of zero means arrival-rate confirmation is pending.
Capture starts with paired slots, as found on C3/C5/C6, and measures the raw word rate
rather than comparing slot values; this also handles IM73D122 data and constant input.
Startup backlog and DMA-overrun windows are excluded. Software bit order starts MSB-first
and can be corrected from the microphone signal after stride confirmation. Detection state
resets when capture restarts. Running at 160 MHz is recommended for software PDM.

Use **512 samples per packet on C3/C5**. Larger buffers consume more RAM; the audio reserve
now adapts to available internal heap, normally retaining 80 KiB for Wi-Fi/TCP plus the producer task
and allocation overhead. If that cannot fit, C5 alone may use the minimum reserve while still
retaining 64 KiB plus the task and overhead; insufficient or fragmented memory is rejected.
The S3/C5 browser-listening queue uses 32 KiB in PSRAM when available, or 4 KiB
(about 43 ms at 48 kHz) with the internal-memory fallback; other boards use 16 KiB.
A slow browser listener is disconnected so it cannot block RTSP. C6/S3 retain the full reserve when memory permits. The API reports
its actual size as `rb_capacity_bytes` and recommends 512 on C3/C5 via
`buffer_size_recommended`. Saved packet sizes are preserved when keeping saved values or using the legacy microphone API. If you previously selected
2048, change it to 512 on C3/C5 or apply **Recommended settings**.

All microphone paths now use integer gain and high-pass processing (`audio_dsp: "integer"`).
PDM retains 48 kHz and a fixed PCM scale; adjust Gain for PDM, while I2S Shift remains I2S-only.
When upgrading from the v15 PDM firmware, use **new Gain = old Gain × 2^(16 − old Shift)**
as a starting point and check clipping: Shift 12 / Gain 1.5 becomes Gain 24. Saved Gain is
not changed automatically.
ESP-IDF documents [raw PDM capture](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-reference/peripherals/i2s.html)
and its MHz clock-rate configuration separately from PCM output sample rates.

Changes are marked **Unsaved** only while they differ from the saved values. Returning to
the saved value clears the marker and the section's discard button automatically; numeric
formatting such as `1.50` versus `1.5` does not count as a change. Status refreshes retain
actual drafts, including edits made while a save is still pending.

### Recommended microphone settings

Changing **Microphone format → Apply** opens a dialog before writing anything. It shows
saved and recommended values for the detected board. Choose **Use recommended and save**,
**Keep saved values and save**, or **Cancel**. **Recommended settings** opens the same
review for the currently selected microphone. Pending audio edits are replaced only when
applying the recommendations; keeping saved values leaves other pending edits available.
PDM always sets 48 kHz and ignores I2S Shift, including when keeping saved values.

The firmware owns one profile list used by both the preview and the save operation:

| Microphone | Boards | Rate | Gain (starting value) | Packet size | I2S Shift | HPF | CPU change |
|---|---|---|---|---|---|---|---|
| ICS-43434 / INMP441 | C3/S3/C5/C6 | 48 kHz | 1.5 | 512 | 12 | On, 500 Hz | Keep current |
| Adafruit SPH0645 | C3/S3/C5/C6 | 48 kHz | 1.5 | 512 | 12 | On, 500 Hz | Keep current |
| PDM IM73D122 | C3/C5/C6 | 48 kHz | 24 | 512 | Unused; stored I2S value retained | On, 500 Hz | 160 MHz |
| PDM IM73D122 | S3 | 48 kHz | 24 | 512 | Unused; stored I2S value retained | On, 500 Hz | Keep current |

512 samples is a common starting point compatible with BirdNET-Pi UDP, and is especially
important on C3/C5. The v15.6 reference recommends 48 kHz and 160 MHz for software PDM.
A later integrated-firmware report supplies the Gain 24 starting value (old Shift 12 /
Gain 1.5). Adjust Gain using Listen and the level/clipping indicator for the actual microphone
and location; it is not an automatic level calibration. HPF 500 Hz and I2S values use this
project's existing defaults. The profile is optional and does not run on boot or firmware update.

The complete microphone/profile change restarts audio once and saves only after success.
A failed capture or CPU change rolls back the audio parameters. The legacy `mic_format`
API continues to preserve custom settings. Profiles can be read using
`GET /api/microphone_profile?format=0|1|2` and saved using
`POST /api/settings/microphone` with `format` and `settings=recommended|keep`, using
the normal mutation header. Profile CPU value zero means keep the current setting.

**Thermal management → CPU frequency** also offers **240 MHz on S3/C5**. C3/C6 are
limited to 160 MHz. This is an optional performance setting; existing defaults remain
160 MHz. Firmware validates the board capability before applying or loading a saved value.
Higher frequency can increase heat; thermal-protection settings are preserved.

### XIAO Antenna Paths

XIAO ESP32-C6 provides a GPIO-controlled RF switch. Open **Network → Wi-Fi antenna**
in the device UI to select **External (default)** or **Internal (built-in)** and
choose **Save & restart**. The setting is stored separately in NVS and applied
before Wi-Fi starts. Missing settings preserve the previous external-antenna
default; the firmware does not automatically detect an attached antenna.
Changing the antenna may interrupt Wi-Fi access if the selected path has weak signal.

The switch uses:

```text
GPIO3  -> LOW
GPIO14 -> HIGH for external, LOW for internal
```

For the external setting, attach a suitable 2.4 GHz antenna. For the internal setting,
keep the built-in ceramic antenna clear of obstructions. Weak Wi-Fi increases retries,
heat, and the chance of audio dropouts. XIAO ESP32-C3 and XIAO ESP32-S3 expose a U.FL antenna connector without a
documented firmware GPIO switch. XIAO ESP32-C5 has a dedicated Wi-Fi/BT antenna connector; attach
the included antenna before use. The UI selector is shown only on XIAO ESP32-C6;
C3/S3/C5 builds neither expose this control nor drive GPIO3/GPIO14 for antenna selection.

## Runtime Defaults

- Sample rate: 48 kHz
- Audio format: mono 16-bit PCM/L16
- Microphone format: ICS-43434 / INMP441 (Philips I2S)
- Gain: 1.5
- Buffer: 512 samples
- I2S shift: 12 bits
- High-pass filter: ON, 500 Hz
- Wi-Fi TX power: 15 dBm
- CPU: 160 MHz
- Thermal shutdown: 80 C, protection ON
- Max RTSP clients: 2
- mDNS: ON
- Time sync: ON
- XIAO ESP32-C6 antenna path: external by default; configurable in Network and stored across restarts
- XIAO ESP32-C3/S3/C5 antenna path: no firmware GPIO switching
- OTA password: none in the default public build

The sample-rate setting accepts 8,000-192,000 Hz. 192 kHz is allowed by the UI/API for users who
want to test ultrasonic-capable microphones, but bandwidth, CPU load, I2S stability, and BirdNET
backend support should be verified on the target setup.

## Web UI

The Web UI runs on port **80**.

![Web UI](../assets/webui2.png)

*Development screenshot (v1.27); interface included in v1.30.*

The header contains **Listen / Stop**, **Playback volume (0–150%)** and a brief
microphone, RTSP, Wi-Fi and temperature overview. Each link opens the relevant section.
All nine sections start collapsed on every page load; opening one leaves the others
unchanged. Background refresh preserves expansion state and pending edits.
Edits remain marked **Unsaved** until explicitly applied or discarded. Every save
reports its result; a failed request keeps the draft. **Factory reset** asks for
confirmation and lists what is reset.

- **Streams**: `/audio1` and `/audio2`, target system, transport, URLs, client states and shared RTSP client limit.
- **Stream health**: microphone signal, audio load, recent/total DMA overruns, capture state, I2S errors, TCP counters, buffer drops and last stop. Lifetime counters are informational; old errors do not keep a current warning active.
- **Audio**: microphone, signal level, gain and HPF, followed by **Advanced audio settings** for sample rate, RTP packet size and I2S shift. **Audio per packet** is packet duration, not total listening delay. Both RTSP streams share these settings. PDM fixes the sample rate at 48 kHz and disables unused I2S Shift.
- **Time & Schedule**: NTP, time zone, streaming window and optional deep sleep.
- **Network**: IP, RSSI, access point selection, Wi-Fi transmit power, mDNS and connection actions.
- **Thermal management**: current/peak temperature, shutdown limit, cooling recovery, latch/history, acknowledgement and CPU frequency.
- **Recovery & Maintenance**: microphone auto-recovery, output-rate warnings, warning check interval, scheduled reboot and service actions. A slow network does not restart a healthy microphone; automatic recovery requires stopped capture reads and has a cooldown.
- **MQTT & Home Assistant**: broker connection, publishing, discovery and connection status.
- **Logs**: device log and download.

Microphone and thermal warnings remain visible above the sections. Firmware updates are
available through **Firmware update** in the header or directly at `/ota`.
The overview distinguishes waiting for a client, disabled streams, a scheduled pause
and cooling. **Save schedule** applies the whole streaming window and sleep choice
together. **Save connection** applies the MQTT broker settings together; an empty
password keeps the saved password, and **Clear saved password** explicitly removes it.

## Firmware Update Without USB

Open:

```text
http://<device-ip>/ota
```

You have two choices:

1. **Automatic update**: use this when the device has internet access. The device downloads
   the board-specific app-only firmware from `esp32mic.msmeteo.cz` and installs it.
2. **Upload compiled file**: use this when the device has no internet access. Upload the app-only
   `.bin` file from your computer. The current release file is
   the matching app-only file from `../manual-ota-firmware/`.

Automatic update uses a board-specific stable plain HTTP URL, for example
`http://esp32mic.msmeteo.cz/updates/firmware-app-c3.bin`. The build/deploy workflow replaces that alias with
the latest published app image on every release. The URL is fixed by the compiled board profile and
cannot be edited in the OTA page, preventing a C3/S3/C5/C6 device from pulling another board's image.
Unsupported generic builds must use manual OTA with a matching app-only file.

### One-time C3/C6 preparation (v1.35)

Update from the older firmware to v1.35 normally. You do **not** need a Prepare
button on the old version: this update installs it.

Then open **Firmware update**, click **Prepare device**, confirm and keep power
connected. Copying, restarts and reconnection are automatic. Wait for
**Preparation complete**. Settings are kept; no additional upload is needed.

Until preparation finishes, the new firmware blocks another automatic update or
manual upload and displays:

> Please prepare your device first using the Prepare device button above.

Your microphone continues working if you postpone it. Once prepared, future
updates work normally and the step is not repeated. S3/C5 and devices already
using the larger layout do not need preparation. Valid custom layouts keep
their existing OTA behavior instead of requiring an unavailable tool.

Keep the device powered during preparation. A power failure during the flash
rewrite can require USB recovery. Older devices continue to receive v1.35 as a permanent bridge, even after larger
releases are published. New firmware uses `/updates/ota-version.txt` and
`/updates/firmware-app-<board>.bin` to reach the latest version.

The server must provide these files over plain HTTP without redirects, with an octet-stream content
type and a valid content length. HTTPS/TLS does not fit in the tight XIAO ESP32-C3/C6 default app
partitions. `firmware-app.bin` remains a C6 compatibility alias for older firmware.

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
- First setup: BirdNETMic welcome page.
- Initial setup AP is open; completed setup and fallback use a random per-device password.
- Wi-Fi trials time out after 25 seconds; automatic retries use a 30-second interval.
- Fallback AP starts after 15 seconds offline. The welcome page has no reboot timeout.
- mDNS can be enabled/disabled in the Web UI.
- Hostname can be changed via API: `key=mdns_hostname&value=esp32mic-garden`.
- mDNS often fails on isolated/guest Wi-Fi or inside Docker containers; use device IP in those cases.
- NTP sync runs on boot, retries every hour until synced, then refreshes every 6 hours.
- Time zone can use a manual UTC offset (including 15-minute increments), automatic presets for Central Europe, New Zealand mainland, UK, US daylight-saving zones, and Sydney/Melbourne, or a custom POSIX daylight-saving rule. Existing settings stay on manual offset after an update.
- In the Web UI, choose the microphone's location. The manual UTC offset or advanced custom rule appears only when that option is selected.
- NTP sets the clock; the selected time zone controls local timestamps and schedule windows. The API uses `time_zone_mode=0..9`, `time_zone_rule=<POSIX rule>` for mode 9, and reports `effective_time_offset_min`. For example, US Eastern is `EST5EDT,M3.2.0/2,M11.1.0/2`.
- Save a valid custom rule before selecting mode 9. Custom rules use recurring `Mmonth.week.weekday` start/end dates.
- After leaving mode 9, clearing the custom rule field restores the default `UTC0` value.
- POSIX rules cover recurring annual transitions. Some regions have irregular or changing rules; check the displayed local time after selection and update the rule if local law changes.
- If time is unavailable, logs fall back to uptime timestamps.

### Stream Schedule And Deep Sleep

Stream schedule is configured in **Time & Schedule**.

- Cross-midnight windows are supported, for example `22:00-06:00`.
- If time is invalid, schedule policy is fail-open: streaming stays allowed.
- The UI rejects an enabled schedule with equal start/stop times. If such a window was saved previously, streaming stays blocked until corrected.
- Optional deep sleep can run outside the stream window only when time is valid.
- Deep-sleep wake timing uses the next local schedule start across daylight-saving changes; the clock may still need NTP after waking.
- Deep sleep is blocked during startup grace, with active clients, or without valid time.

API keys:

```text
stream_sched=on|off
stream_start_min=<0..1439>
stream_stop_min=<0..1439>
deep_sleep_sched=on|off
```

## Listen in the browser

In the page header, press **Listen**. The device streams the same processed mono PCM used
by RTSP, including gain and HPF, for both I2S and hardware/software-decoded PDM microphones.
Playback uses the active sample rate; PDM remains fixed at 48 kHz. Playback volume changes only
browser playback, not the microphone gain or the BirdNET stream.

One browser can listen at a time, independently of the RTSP client limit. Keep the page
open and visible; Stop, leaving the page or hiding the tab ends playback. A slow browser
connection affects only the preview. Brief interruptions reopen the audio connection
automatically while preserving playback volume; consecutive failures have a bounded
retry limit. Stop cancels recovery immediately. Audio reconfiguration, OTA, Wi-Fi
reconnect, server OFF, scheduling and thermal shutdown may close or pause the preview;
press Listen again if it cannot recover. Intentionally turning Wi-Fi off stops Listen.

The preview uses HTTP port **81**, `/listen.wav`, with a separate bounded queue
as described in the memory table above. Busy replies remain readable in Chrome;
after a quick Stop → Listen the browser briefly retries the previous connection.
Open the device's HTTP Web UI directly; network filtering must permit both ports 80 and 81.
Cross-origin browser access is limited to the matching device page on port 80.

## Microphone signal health

The header and **Stream health → Microphone signal** report `checking`, `ok`, `flatline` or `unavailable`.
The detector examines raw I2S samples or decoded PDM PCM **before user gain, shift and HPF**.
Twenty seconds without changing values (including all-zero input) produces a warning;
normal quiet recordings still contain microphone noise and count as a changing signal.
This is a signal-change check, not proof of correct wiring, alignment or sound quality.

No microphone reset is triggered by this warning. A changing signal clears it automatically.
Restart/reconfiguration resets the detector; absent/stale capture reports unavailable.
`GET /api/audio_status` and MQTT state include `mic_signal` and `mic_flatline_sec`.
The audio API also reports `browser_listening`.

## Audio diagnostics

The **Stream health** table reports **Audio processing (estimate)**. This measures time spent processing captured audio, excluding blocking microphone-read waits, over two-second windows. Scheduling delays can inflate the estimate; it is not total chip CPU usage. The peak resets when audio capture restarts. The API fields `audio_cpu_pct` and `audio_cpu_peak_pct` are `null` until a complete window is available, while capture is stopped, or after two seconds without a successful read; `audio_cpu_valid` states whether they are usable.

**I2S DMA overruns** count receive buffers overwritten before capture could read them. They are separate from ring-buffer drops and RTSP/network counters. `dma_overruns_total` lasts until device reboot; `dma_overruns_1min` is a rolling count sampled about once per second. A delayed poll attributes accumulated increments to that poll, so recent counts and `dma_overrun_last_age` have polling precision. Counts pause during capture reconfiguration. If callback registration fails, `dma_overruns_available` is false and numeric fields are `null`, rather than claiming zero loss. Diagnostics do not reset I2S or change the audio signal.

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

Grouped settings use URL-encoded bodies and validate all fields before saving:

- `POST /api/settings/schedule`: `enabled=on|off`, `start` and `stop` in local minutes since midnight (0–1439), and `sleep=on|off`. An enabled window must have different start/end times; sleep requires an enabled schedule.
- `POST /api/settings/mqtt`: `enabled=on|off`, `host`, `port`, `user`, `password`, and `clear_password=on|off`. An empty password with clearing off retains the saved password. Clearing and supplying a replacement together is rejected. The broker reconnects once after a successful save.

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
key=mic_format&value=1
key=hp_enable&value=on
key=hp_cutoff&value=600
```

Target values:

```text
0 = BirdNET-Go
1 = BirdNET-Pi
```

Microphone-format values are `0` for ICS-43434/INMP441 Philips I2S (default) and `1` for Adafruit
SPH0645 MSB / left-justified.

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

`/api/audio_status` includes audio pipeline diagnostics:

```text
producer_running
i2s_error_count
rb_capacity_bytes
rb_chunks
rb_drops
rb_flushes
rtsp_write_stalls
rtsp_write_timeouts
```

## MQTT And Home Assistant

MQTT settings are available in the Web UI and API.

- Telemetry topic: `<topic_prefix>/state`
- Availability topic: `<topic_prefix>/availability`
- RTSP server command: `<topic_prefix>/cmd/rtsp_server` with `ON` or `OFF`
- Reboot command: `<topic_prefix>/cmd/reboot` with `PRESS` or `REBOOT`
- Publish interval: default 60 s, range 10-3600 s
- Important events such as stream start/stop and connection changes request an immediate update;
  requests are coalesced and published after audio processing in the main loop.

Home Assistant MQTT Discovery creates entities for runtime values such as RSSI, uptime, heap,
temperature, stream state, packet rate, client count, reboot reason, restart counter, firmware
version/build, and Wi-Fi reconnect count.

MQTT password is stored in device flash in plain text NVS.

## Build And Flash Manually

### Recommended: Bash and Arduino CLI

Use Bash (Linux/macOS or Windows WSL), Python 3 and Arduino CLI **1.5.0**.
Run commands from the repository root, not from the sketch directory.
For first-time setup, add the [Espressif package index](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
and install the core/libraries pinned in `build-dependencies.json`:

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
python3 tools/check_build_dependencies.py --install
```

Example for XIAO ESP32-C6:

```bash
tools/compile_firmware.sh --fqbn esp32:esp32:XIAO_ESP32C6 \
  --output-dir esp32-birdnet-mic/build/c6
```

| Board | FQBN |
|---|---|
| XIAO ESP32-C3 | `esp32:esp32:XIAO_ESP32C3` |
| XIAO ESP32-S3 | `esp32:esp32:XIAO_ESP32S3` (the helper adds `PSRAM=opi`) |
| XIAO ESP32-C5 | `esp32:esp32:XIAO_ESP32C5` (the helper adds `PSRAM=enabled`) |
| XIAO ESP32-C6 | `esp32:esp32:XIAO_ESP32C6` |

Change the FQBN and output folder together when building another board.
The helper automatically enables LTO at compilation and linking for C3/C6 and isolates their
Arduino core build artifacts. It adds `PSRAM=opi` for S3 and `PSRAM=enabled` for C5,
so Streaming and Listen can use the boards' 8 MB PSRAM. The all-board packaging script
and GitHub Actions use the same helper, so no per-release flag changes are needed.

For OTA, open **Firmware update -> Upload compiled file** on the device and select
`esp32-birdnet-mic/build/c6/esp32-birdnet-mic.ino.bin` from this example.
Choose the image for the correct board. The `.ino.bin` is app-only; `.merged.bin` is for USB,
not OTA. Uploading an app image alone keeps the existing partition layout;
follow **Prepare device** when prompted on C3/C6 before another OTA. Automatic
download URLs are unchanged.

For an initial USB installation, use the same output directory explicitly:

```bash
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32C6 --port <PORT> \
  --input-dir esp32-birdnet-mic/build/c6 esp32-birdnet-mic
```

When changing the embedded Web UI, first regenerate its assets as described under
[Web UI Development](#web-ui-development). Generated headers are already included for an ordinary build.

### Arduino IDE

1. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
2. Install an ESP32 Arduino core with support for the selected XIAO target.
3. Select the matching board: *XIAO_ESP32C3*, *XIAO_ESP32S3*, *XIAO_ESP32C5*, or *XIAO_ESP32C6*.
   For *XIAO_ESP32S3*, set **Tools → PSRAM → OPI PSRAM**.
   For *XIAO_ESP32C5*, also set **Tools → PSRAM → Enabled**. Without it the C5 still works,
   but with the adaptive internal-RAM audio buffer and listening queue (4 KB).
4. Compile and upload over USB.

Keep `build_opt.h` next to the `.ino`; it reduces the image size by disabling
unused C++ exception/unwind metadata. Ordinary Arduino IDE or direct
`arduino-cli compile` builds do not apply the additional C3/C6 LTO settings automatically.
Use the CLI helper above for the same settings as the packaged firmware and CI.

### PlatformIO

Typical targets are board-specific Arduino environments, for example `env:xiao_esp32c6`:

```bash
pio run -t upload
```

## Web UI Development

Install the pinned generator dependencies from the repository root:

```bash
python3 -m pip install -r tools/requirements-webui.txt
npm ci --prefix tools --ignore-scripts
```

Terser minifies inline JavaScript while preserving names used by HTML handlers.
Zopfli compresses the resulting HTML and the OTA assets with 15 iterations.
Readable sources stay in `webui/`; decompression happens in the browser. The device
does not need a decompressor or internet access. Tests check reproducibility and
exercise both the source and the actual embedded page.

From the repository root, regenerate after editing any browser asset:

```bash
esp32-birdnet-mic/tools/gen_webui_gzip_header.sh
```

This updates `WebUI_gz.h` and `OtaAssets_gz.h` from `webui/index.html`, `webui/ota.css`
and `webui/ota.js`. Add `--check` to verify that generated headers match without rewriting them.

## Persisted Configuration

Most runtime settings are stored in NVS namespace `audio` through ESP32 Preferences.

Main keys:

```text
sampleRate       Audio sample rate
gainFactor      Audio gain
bufferSize       Samples per packet/buffer profile
shiftBits        I2S right shift before gain
micFormat        I2S sample alignment: 0 = Philips, 1 = MSB / left-justified
hpEnable         High-pass enable
hpCutoff         High-pass cutoff Hz
wifiTxDbm        Wi-Fi TX power
mdnsEn           mDNS enable
timeSyncEn       NTP enable
timeOffset       Local offset in minutes
timeZone         0 = fixed offset, 1 = Central Europe, 2 = New Zealand,
                 3 = UK, 4..7 = US Eastern/Central/Mountain/Pacific,
                 8 = Australia Eastern, 9 = custom POSIX rule
timeZoneRule     Custom POSIX daylight-saving rule (mode 9)
strSchedEn       Stream schedule enable
strSchStart      Stream window start minute
strSchStop       Stream window stop minute
deepSchSlp       Deep sleep outside schedule window
autoRecovery     Stopped audio-capture recovery enable
thrAuto          Automatic threshold mode
minRate          Manual minimum recent output rate for a warning
checkInterval    Recovery check interval in minutes
schedReset       Scheduled reset enable
resetHours       Scheduled reset interval
ohEnable         Thermal protection enable
ohThresh         Thermal shutdown threshold C
ohAuto           Automatic thermal recovery enable (default OFF)
ohRecovery       Persisted retry history, warning and recovery state
ohLatched        Persisted thermal latch (legacy compatibility)
```

Apply changes through Web UI or API. Audio-related updates call `restartI2S()` when needed.
Current validation ranges include `sampleRate=8000..192000`, `bufferSize=256..8192`, and
`micFormat=0..1`.

## RTSP Implementation Notes

- I2S capture and audio processing run in a producer task.
- RTSP output consumes processed PCM blocks from a FreeRTOS ring buffer and packetizes them as RTP.
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
- With thermal protection explicitly OFF, acknowledge a saved shutdown immediately, even if the sensor is unavailable. OFF alone does not restore streaming; acknowledgement is a separate action. With protection ON, normal cooling is still required.
- OTA pauses capture and I2S/PDM clocks until reboot; a failed update restores capture, respecting thermal shutdown and stream schedules.
- Temperature monitoring checks driver errors and switches between supported sensor ranges; readings remain approximate chip temperatures.
- Thermal protection disables RTSP and capture at the configured limit. Optional automatic recovery waits five continuous minutes at least 10 C below the limit. The third shutdown in a rolling 24 hours, an emergency shutdown (limit + 10 C), or a legacy latch requires manual acknowledgement after cooling. History and blocks survive reboot; without a valid clock, unknown event ages are conservatively counted for 24 hours after boot. Sensor errors reset cooling; manual server stops cancel pending automatic recovery. Recovery respects the stream schedule. The warning stays visible until acknowledged, and last-shutdown details remain available.

## Security

- Keep the device on a trusted LAN.
- Do not expose HTTP, RTSP, or OTA to the public internet.
- The default public build has no OTA password.
- If you build your own firmware for untrusted networks, set an OTA password before publishing it.
- Mutating API endpoints require `POST` and `X-ESP32MIC-CSRF: 1`, but read endpoints are not globally authenticated by default.

## Known Limitations

- No TLS or built-in user authentication for the Web UI/API.
- mDNS depends on multicast support in your LAN and often does not work across VLANs, guest networks, or Docker bridge networks.
- Sense support remains experimental.

## Credits

- Author: **@Sukecz**
- Thanks to everyone helping improve BirdNETMic.

## License

This firmware is released under the MIT License. See `../LICENSE`.
