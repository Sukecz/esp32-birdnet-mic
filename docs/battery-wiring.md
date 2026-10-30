# Battery wiring and protection


Use a **3.7 V nominal Li-ion/LiPo cell or 1S parallel pack**, charged to
**4.2 V maximum**, with suitable battery protection. Parallel cells increase
capacity without changing voltage; use matched cells at the same voltage.
Series packs (2S or higher) are not supported.

### Connection diagram

<p align="center">
  <a href="../assets/battery-connection.png"><img src="../assets/battery-connection.png" alt="Li-ion cell connected through a 1S BMS and positive-lead fuse to XIAO ESP32-C6 BAT pads, with a 100 kΩ + 100 kΩ voltage divider to D0/A0" width="640" /></a>
</p>

*The updated illustration shows XIAO ESP32-C6. Click it for full size.
Check the BAT pad polarity on your own board. C5 uses the same protected
battery connection but already has its measurement divider built in.*

For the common-port 1S protection board shown:

| Connection | Destination |
|---|---|
| Cell / pack positive | BMS **B+** |
| Cell / pack negative | BMS **B−** |
| BMS protected **P+** output | **Fuse → XIAO BAT+** |
| BMS protected **P−** output | **XIAO BAT− / GND** |
| External measurement divider, when required | Across **XIAO BAT+ and BAT−**, after the protection and fuse |

Keep the XIAO and divider ground on **P−**; connecting them directly to raw
cell **B−** can bypass the BMS protection. BMS terminal names and arrangements
vary: follow the documentation for the actual module.

### BMS, fuse and charging

The illustrated assembly uses a **1S BMS rated 3 A** and a **3.5 A fuse** in
the positive output lead. These are example component values, not a universal
rating recommendation. A 3.5 A fuse does not make a 3 A BMS suitable for higher
continuous current and is not an exact 3.5 A current limiter.

- Choose the BMS for the cell chemistry, pack current and its separate charging
  and discharge limits; a “3 A” label alone does not specify all of these.
- Select the fuse and holder for the wire size, connectors, normal current,
  expected fault current and the fuse's time-current curve. Put the output fuse
  close to the BMS output; keep cell-to-BMS leads short and insulated.
- Charging current is set by the charger and limited by the cell and BMS.
  Increasing the fuse rating does not increase charging speed. The official
  [XIAO C5 schematic](https://files.seeedstudio.com/wiki/XIAO_ESP32C5/res/Seeed_Studio_XIAO_ESP32C5.pdf)
  specifies approximately **200 mA** for its onboard charger; check the exact
  board revision for other models.
- The BMS and fuse provide hardware protection. Enable the configurable
  **Low-battery protection** in the Battery section for automatic Recording
  Stop & save, followed by staying awake or deep sleep. Choose a voltage that
  leaves enough reserve to save before the pack protection cuts power.
- In Recording, **Turn Wi-Fi off until restart** disables both AP and home Wi-Fi
  while SD recording and battery/thermal monitoring continue. The switch-off
  delay is configurable; reset or power-cycle to reconnect.

See [Littelfuse's fuse selection guide](https://www.littelfuse.com/assetdocs/fuseology-fuse-facts-and-fuse-selection-guide?assetguid=be92b28c-1f63-4f48-a505-55c582b7c1ae)
for fuse rating, temperature, fault-current and opening-time considerations.

### Voltage monitoring by board

| Board | Measurement connection |
|---|---|
| **XIAO ESP32-C5** | Built-in **100 kΩ + 100 kΩ** divider. Connect the protected pack to **BAT+ / BAT−**; no external divider is needed. |
| **XIAO ESP32-C3 / S3 / S3 Sense / C6** | External **100 kΩ + 100 kΩ** divider; midpoint to **D0/A0**. See the guide for the board-specific GPIO. |
| **M5Stack AtomS3U** | **Unsupported** for battery monitoring. |

For C3/S3/C6, connect one resistor from the protected, fused **BAT+** to
**D0/A0**, and the other from **D0/A0** to **BAT− / GND**. Never connect BAT+
directly to an ADC pin. The divider input is separate from our microphone and
SD wiring.

### Enable monitoring and calibrate

Open **Battery** on the main page, enable
**Enable battery monitoring** and press **Save**. The same setting is available
in **Device configuration**. Monitoring is off by default; when enabled, the
top overview shows **Battery: ~65% · 3.85 V** in both Streaming and Recording.
The Battery section also shows the wiring for the selected board.

To calibrate, compare stable readings with a multimeter across the XIAO's
**BAT+ / BAT−** at the same time. Enter the meter voltage and press **Calibrate**.
The saved correction (up to **±10%**) adjusts both voltage and the estimated
percentage, and survives restart/OTA. **Reset calibration** restores 0%.

Percentage is approximate. Battery presence, USB power and charging are not
automatically detected; USB power can produce a reading without a battery.
See the [battery guide](battery-monitoring.md) for GPIOs, APIs and
hardware-validation limits.
