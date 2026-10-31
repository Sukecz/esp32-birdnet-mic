# Recording away from home

BirdNETMic can collect woodland, garden and other environmental sounds without a
computer or router. Our recommended new assembly is **XIAO ESP32-C5 + IM73D122 PDM
+ Adafruit microSD BFF #5683**. See [parts](parts.md) and [wiring](../README.md#microphones-and-wiring).

To choose your own device Wi-Fi password, open **Network → Device Wi-Fi → Change password** (also available in Device configuration). Use 8–63 characters without accents, then **Save password**. Stop recording before saving. If connected through device Wi-Fi, reconnect with the new password afterward. Available from firmware 2.03; earlier firmware keeps its existing generated password.

## Before leaving

1. Install the matching firmware and select Recording in Device configuration as
   the default mode. Choose standalone Device Wi-Fi for use without a router.
2. Fit a FAT32 card with power disconnected. Check SD status and use Listen to
   confirm microphone sound. Stop Listen after the sound check.
3. Choose **Recording duration → Until stopped** or **Set a duration** (minutes,
   up to 10080 = seven days). Choose **Start a new file every** (1–120 minutes).
   Save the settings. Longer uninterrupted sessions use Until stopped, subject
   to available power and storage.
4. If wanted, enable **Start recording after power-on**. This is separate from
   choosing the default mode. Enable battery monitoring and configure low-battery
   protection with enough reserve to finish saving before hardware power cutoff.
5. Make a short test recording, Stop & save, and play the WAV on a computer.
   Reinsert the card with power disconnected. Start the intended field session.
6. Check the recording status before leaving. Protect the assembly from water,
   condensation and direct sun without blocking the microphone.

## Wi-Fi off and getting back in

**Turn Wi-Fi off until restart** acts on the current session. Its configurable
0–60 second delay lets you see the request before connection is lost. Both home
Wi-Fi and Device Wi-Fi turn off. Audio capture, SD writes, thermal and battery
protection continue. Listen, remote control, MQTT and OTA need Wi-Fi and become
unavailable. Browser values are frozen, not live confirmation of continued recording.

A normal offline session enters deep sleep after it finishes saving, or after
error cleanup. A thermal pause with recovery pending does not count as definitive
completion. If automatic thermal recovery is enabled, eligible recovery starts a
new file after cooling. Do not assume a permanently stopped session will resume.

**There is no ordinary wake timer or remote way to turn Wi-Fi back on.** Reset or
power-cycle the device. A reset during recording may leave the current `.wav.part`
unfinished; completed earlier segments remain on the card. For a planned finish,
set a duration before switching Wi-Fi off and allow time for finalization.
On restart, the saved default mode and auto-start setting apply again.

With Wi-Fi left on, reconnect and use **Stop & save**, wait for Saved, then power
off before removing the card. Read `recordings/` on a computer. The firmware
checks WAV headers and length before renaming completed files; that check is not
full audio-content verification. Errors preserve partial files, not guaranteed
recoverable recordings. Existing recordings are never automatically deleted.

## Storage and battery calculations

At 48 kHz × 2 bytes × one channel, PCM uses 96000 bytes/s, 345.6 MB/hour or
8.2944 GB/day. WAV headers, filesystem space, logs and reserve reduce usable time.
The [main table](../README.md#recording-outdoors-days-of-sound-on-microsd) uses decimal
card capacities. Other sample rates scale proportionally. FAT32 is the documented
filesystem; 64/128 GB examples are arithmetic, not physical qualification. Sense's
[manufacturer guide](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
specifies FAT32 and up to 32 GB. Back up data before any reformatting.

User measurements on C5, 80 MHz, battery disconnected, at 5.159 V USB:
Recording used 78–95 mA with Wi-Fi and 34–50 mA without it, depending on four
8/32 GB cards. Wi-Fi-off power is therefore 0.175–0.258 W. These are USB readings,
not direct battery-current measurements, and do not apply automatically to other boards.

Planning formula: `days = capacity_Ah × 3.7 V × 0.80 / power_W / 24`.
The 80% budget is an illustrative allowance, not measured BAT-path efficiency.
One 3000 mAh cell gives 1.4–2.1 days; four parallel cells give 5.7–8.4 days.
Temperature, cell aging, cutoff and the power path may change this substantially.
The onboard charger is intended for small batteries; a large pack takes much longer
to charge. See [battery protection and charging](battery-wiring.md).

## What has actually been tested

Firmware 2.03 corrects the internal-memory budget used to start
Recording with a PSRAM buffer and keeps mDNS OFF across restart. Host regressions
cover the low-memory AP case and allocation failures. Additional S3 checks with
AP and mDNS enabled confirm buffer preparation and capture restart under low
internal memory, without an attached card. A fresh C5 field test with PDM, AP,
mDNS and an actual SD card is still required for this change.

A C5 with Adafruit BFF and 32 GB FAT32 card recorded for **9 h 6 min 52 s** at
48 kHz, CPU 80 MHz and Wi-Fi ON. Ten WAV files were finalized (about 3.15 GB),
with no observed SD I/O errors, dropped recording samples or new I2S/DMA errors.
Separate short C5 tests exercised Wi-Fi OFF and low-voltage save/sleep using a
simulated low reading. Neither proves real pack discharge or multi-day endurance.
Later logging changes had shorter physical retests; the overnight result does not
qualify every subsequent build. Other XIAO SD paths need physical tests; Sense is
experimental. Choose 160 MHz as the usual software-PDM starting point; the 80 MHz
measurement and overnight run describe this specific tested C5 assembly.
