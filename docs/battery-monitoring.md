# Battery: connection and monitoring

Available in **firmware 2.00**, in both Streaming and Recording.

Open **Battery** on the main page, select **Enable battery monitoring** and press
**Save**. The same setting is available in **Device configuration → Battery
monitoring**, with **Save changes** (or **Finish setup** on a new device).
In Device configuration, selecting the checkbox opens a short wiring guide; use
**Battery wiring** to open it again. The main Battery section shows wiring
inline. Monitoring is off by default.

The main UI shows **Battery: ~65% · 3.85 V** directly in its top status overview;
there is no extra page or click to see the readings. Disabling monitoring hides
the indicator. Invalid or stale readings show **Battery: unavailable**, rather
than a false 0%. Monitoring does not change the operating mode or stop recording.

## Battery and pack type

Use a **3.7 V nominal Li-ion/LiPo battery, charged to at most 4.2 V**. This can
be one cell or a **1S parallel pack** (1S2P, 1S3P, etc.). Parallel cells increase
capacity, while pack voltage stays the same; no cell count or capacity setting
is needed. Use a suitably protected pack with matched cells; do not directly
parallel cells at different voltages.

Series packs (2S or higher), LiFePO4 and higher-voltage Li-ion chemistries are
not supported by this wiring/percentage profile. Feed the board's BAT pads from
the protected, fused pack output with the correct polarity. Share the protected
P− / XIAO BAT− ground with the measurement circuit, not the raw cell B− terminal.
Never connect BAT+ directly to an ADC input.

## Protected battery connection

![Li-ion cell, 1S BMS, positive-lead fuse and external C6 voltage divider](../assets/battery-connection.png)

The image shows a common-port **1S BMS**: cell terminals connect to **B+/B−**;
**P+ → fuse → XIAO BAT+**, and **P− → XIAO BAT− / GND**. The external divider
connects across the protected, fused XIAO BAT supply. Connecting device ground
to raw cell B− can bypass the protection. Check the actual module's pin labels.

The illustrated assembly uses a **3 A BMS** and a **3.5 A output fuse**. These
values describe the example; they are not a general sizing recommendation.
A fuse does not increase the BMS's current rating or set the charging current.
Choose it for the wiring, fault current and its time-current characteristic;
check the actual BMS's charge and discharge limits and the cell specification.
Place the output fuse close to the BMS output and protect the cell-to-BMS leads.
The [main README Battery section](../README.md#battery) covers recommendations
and charger limits, with manufacturer references.

## Wiring by board

| Board / selected profile | Measurement input | Divider |
|---|---|---|
| XIAO ESP32-C5 | Internal GPIO6, enabled by GPIO26 | Built in: 100 kΩ + 100 kΩ; ready to use |
| XIAO ESP32-C3 | D0 / A0, GPIO2 (ADC1) | External: 100 kΩ + 100 kΩ |
| XIAO ESP32-S3 / Plus, XIAO ESP32-S3 Sense | D0 / A0, GPIO1 (ADC1) | External: 100 kΩ + 100 kΩ |
| XIAO ESP32-C6 | D0 / A0, GPIO0 (ADC1) | External: 100 kΩ + 100 kΩ |
| M5Stack AtomS3U | Unsupported | Monitoring cannot be enabled |

**C5:** connect the battery to BAT+ and BAT−. No external resistors are needed.
The firmware enables the built-in measurement circuit only while sampling.

**C3 / S3 / C6:** use two **100 kΩ resistors**:

```text
BAT+ ── 100 kΩ ──┬── D0 / A0
                 │
               100 kΩ
                 │
BAT− / GND ──────┘
```

Both divider types halve the voltage: 4.2 V at the battery
becomes 2.1 V at the ADC. The external divider draws about 21 µA at 4.2 V.
The S3 / Plus profile uses this external A0 route; this feature does not use
any separate onboard battery monitor provided by a particular Plus revision.

*The connection illustration above shows C6. Check BAT pad polarity on your own
board. C5 needs no external divider.*

Optional: if readings are noisy, a 100 nF capacitor from A0 to GND can help;
1% resistors can reduce divider-ratio error. Neither is required by the setup guide.

The A0 input is separate from our external microphone pins **D1/D2/D3/D7**,
external microSD pins **D6/D8/D9/D10**, and the Sense profile's built-in microphone
and SD slot. No microphone or SD rewiring is needed.

On **C3**, GPIO2 is also a strapping pin. Seeed uses A0 for its battery example;
startup with the proposed divider still needs physical checks with battery,
USB and no battery before this wiring can be called hardware validated.

## What the readings mean

- Voltage is measured through the ADC's calibrated millivolt API, averaged over
  16 samples, then smoothed. A sample cycle starts approximately every 10 seconds,
  independently of the browser, with one conversion per application-loop step.
  Audio capture does not perform battery reads.
- **~% is a voltage-based estimate**, using a generic nonlinear 4.2 V Li-ion/LiPo
  curve. Load, temperature, cell age and charging affect the result. It is not a
  precision fuel gauge, a runtime estimate or a pack protection circuit.
- Enabling monitoring is a user setting. Voltage alone does **not** reliably
  detect battery presence, USB power, active charging or charge completion.
  A plausible reading under USB power is not proof that a battery is attached.
- Monitoring alone only reports voltage. Enable **Low-battery protection**
  separately to stop and save a Recording session automatically.
- The setting is persistent across restart/OTA and independent of the selected
  operating mode. Unchanged saves do not rewrite the setup configuration.

`GET /api/status` includes a `battery` object with `enabled`, `supported`, `valid`,
`voltage` (volts), `percent`, `raw_voltage` and `calibration_percent`. Invalid, disabled or stale measurements use
JSON `null` for both numeric readings. `GET /api/setup` includes
`battery_enabled` and `battery_supported`; setup saves accept `battery=0` or `1`.
Omitting that field preserves the saved choice.

Voltage accuracy depends on calibration and the complete powered assembly.
Trial-run the intended battery and card before unattended use.

## Hardware references

- [Seeed XIAO C5: built-in battery voltage measurement](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/#check-the-battery-voltage)
- [Seeed XIAO C3: external A0 measurement](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/#check-the-battery-voltage)
- [Seeed XIAO C6: external A0 measurement](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/#reading-battery-voltage)
- [Seeed XIAO S3: battery usage](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/#battery-usage)
- [Espressif C3: strapping pins](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/schematic-checklist.html#strapping-pins)
- [Espressif: ADC noise reduction](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32c3/api-reference/peripherals/adc.html#minimizing-noise)

## Main-page controls and calibration

The main UI has a **Battery** section, available even while monitoring is off.
Its **Enable battery monitoring** switch shares the saved choice with Device
configuration. Reverting an unsaved switch clears Unsaved without writing.
Board-specific wiring is shown in this section: C5 needs only BAT+/BAT−;
C3/S3/C6 need the external 100 kΩ + 100 kΩ divider; AtomS3U is unsupported.

For calibration, enable monitoring and compare a stable reading with a multimeter
measured across BAT+/BAT− at the same time. Enter the meter voltage and press
**Calibrate**. The correction applies to voltage and the estimated percentage;
it is saved across restart/OTA. Calibration uses the uncorrected measurement,
so repeating it does not compound the correction. Corrections are limited to
±10%. **Reset calibration** restores 0%; factory reset also clears calibration.
Calibration does not detect battery presence, charging or USB power.

`POST /api/battery` uses the normal mutation-auth header. Actions are
`action=enable&enabled=0|1`, `action=calibrate&reference=<volts>` and `action=reset`.
Calibration requires monitoring, a fresh valid measurement and a correction
within ±10%. Unsupported boards reject these actions.


## Unattended recording and Wi-Fi switch-off

In **Recording → Battery operation**, save the switch-off delay (0–60 seconds),
start recording, then press **Turn Wi-Fi off until restart** and confirm.
Both the device AP and the home Wi-Fi radio stop. Microphone capture, SD writing,
rotation, battery monitoring and thermal protection continue. The page freezes
its readings and stops polling. Reset or power-cycle the device to restore Wi-Fi;
credentials and startup mode are preserved. The normal action never schedules an
automatic restart or wake-up. A zero-second delay still allows the HTTP response
to be delivered before shutdown.

In **Battery → Low-battery protection**, configure:

| Setting | Range | Firmware default |
| --- | --- | --- |
| Protection enabled | Off / On | Off |
| Stop voltage | 3.00–4.20 V | 3.50 V |
| Confirmation | 0–300 seconds | 30 seconds |
| Recovery margin | 0–0.30 V | 0.10 V |
| After saving | Stay awake / Deep sleep | Deep sleep |
| Wi-Fi switch-off delay | 0–60 seconds | 3 seconds |

The guard uses each fresh calibrated 16-sample batch, without the display's
smoothing. Samples arrive approximately every 10 seconds, so confirmation is
evaluated on the next fresh batch. Readings below the stop voltage start the
confirmation period; recovery above the voltage plus margin cancels it. A missing
or stale reading cannot trigger shutdown and blocks new recordings while the
guard is enabled. Start after power-on waits for the first valid battery reading.

On confirmed low voltage the recorder stops accepting samples, drains queued
PCM, closes and verifies the WAV file, and then performs the selected action.
Deep sleep disables all normal wake sources: reset or power-cycle to wake it.
Stay awake keeps the UI available if Wi-Fi is on; clear the latched stop after
voltage rises above the stop voltage plus recovery margin before recording again.
The guard does not resume a recording automatically after a low-battery stop.
Settings are saved separately from audio, recording and battery calibration.
Settings changes require Stop & save first. Disable the guard before disabling
battery monitoring.

A sudden power interruption or an SD failure can still leave a partial file or
damage the filesystem; losing only the current segment is not guaranteed. The
voltage guard needs enough battery reserve to complete the save and does not
replace the pack's hardware protection. Deep sleep still draws some current and
cannot physically disconnect the battery. Actual runtime and Wi-Fi savings need
current measurement on the complete powered assembly.

`POST /api/field_power` accepts `enabled`, `cutoff_mv`, `confirm_seconds`,
`hysteresis_mv`, `sleep_after_save` and `wifi_delay_seconds`, using the normal
mutation header. `GET /api/status` exposes `field_power`, `wifi_off_pending` and
`wifi_suspended`. `POST /api/recording/action` adds `action=wifi_off` and
`action=battery_acknowledge`.
