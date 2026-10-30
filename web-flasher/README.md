# BirdNETMic Web Flasher — ESP32 Microphone USB Installation and OTA Updates

> **Firmware 2.01:** Streaming and standalone Recording. See the [main guide](../README.md).


Firmware **v2.01**. Candidate in preparation. See the [release validation](../release-notes/2.01-validation.md).

**Firmware: 2.01** (2026-10-30).

Install using [esp32mic.msmeteo.cz](https://esp32mic.msmeteo.cz) in desktop **Chrome or Edge**
with a USB **data** connection (AtomS3U plugs directly into USB-A). The flasher detects the ESP chip family and chooses the image for
XIAO ESP32-C3/S3/C5/C6. AtomS3U and Sense also use the S3 image; chip detection
cannot distinguish these board designs, so select the matching profile after installation.

**XIAO ESP32-S3 Sense / M5Stack AtomS3U:** use the S3 image, then open
**Device configuration → Board** in the device Web UI, select your board and save the configuration.
**AtomS3U** has a built-in microphone; start with Gain 24 and check Listen.
**[XIAO ESP32-S3 Sense](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
is experimental**; its profile also accepts external I2S microphones.

For wiring, microphone selection and feature status, see the [main guide](../README.md).
PDM is supported. C3/C5 should use **512 samples per packet** and **160 MHz**.
Keeping saved values preserves the packet size; **Recommended settings** applies 512.

## Choose the right file

| Purpose | File | Notes |
|---|---|---|
| Update through the device's OTA page | `firmware-app-<board>.bin` | App-only; choose c3, s3, c5 or c6 |
| Exact 2.01 OTA image | `firmware-app-<board>-2.01.bin` | Current versioned app-only image |
| USB web installation | `manifest.json` and its referenced parts | Let the web flasher handle offsets |
| Manual full USB flash | `firmware-<board>.bin` | Merged image; can overwrite settings; never upload through OTA |
| Legacy unsuffixed files | `firmware-app.bin`, `firmware.bin` | **C6 only** |

Normal OTA retains saved settings and the existing partition layout; no USB migration is required.
USB installation may erase settings: choose to keep existing data when updating through the flasher.

### Structure
- `index.html` - main page with the "Flash" button and instructions.
- `manifest.json` - ESP Web Tools manifest, writes only firmware parts without NVS and includes C3/S3/C5/C6 builds.
- `bootloader-<board>.bin`, `partitions-<board>.bin`, `boot_app0-<board>.bin` - system parts for USB web flashing.
- `firmware-<board>.bin` - merged image (bootloader + partition + app), only for manual full flash.
- `firmware-app-<board>.bin` - app-only image for OTA update through the device Web UI.
- `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`, `firmware-app.bin` - compatible C6 aliases.
- `ota-version.txt` - simple text file with the OTA build version.

The manifest intentionally does not use the merged `firmware.bin` at offset `0x0`, because during an
update it would overwrite the NVS area from `0x9000` with `0xff` bytes and erase Wi-Fi and application
settings. ESP Web Tools also treats flashing an unrecognized device as a new installation and erases
the whole flash by default. Because of that, `new_install_prompt_erase` is enabled in the manifest;
when updating, choose to keep existing data.

### OTA update without USB
1. Open **https://esp32mic.msmeteo.cz**.
2. In the **OTA update without USB** section, enter the device IP address, for example `192.168.1.80`.
3. Click **Open OTA update**.
4. On the device page, choose one option:
   - **Automatic update**: the device downloads the latest board-specific app-only firmware from the web.
   - **Upload compiled file**: manually select an app-only `.bin` file.

**C3/C6, starting with v1.35:** install the first update normally, then click
**Prepare device** on the device's update page and confirm. Keep power connected
and wait for **Preparation complete**; restarts and reconnection are automatic,
and your settings are kept. The new firmware requires this step before another
OTA update. The older firmware does not need the button to install v1.35.

S3/C5 and devices already using the larger layout skip this step. New C3/C6 USB
installations built with the v1.35 helper already use the larger layout. During
preparation, a power failure can require USB recovery. Older firmware always receives v1.35 first; after preparation, a second normal
update can install the latest release, even if several versions were skipped.

Automatic download uses a stable plain HTTP URL based on the compiled board profile, for example
`http://esp32mic.msmeteo.cz/updates/firmware-app-c3.bin`. Deployment replaces each stable board alias with
the latest image and verifies it against the versioned artifact. The server must serve these files
without redirects, with an octet-stream content type and valid content length. HTTPS/TLS does not
fit in the tight default XIAO ESP32-C3/C6 partitions. `firmware-app.bin` remains a C6-compatible
alias for older firmware.

### Updates after skipped releases

The original `/ota-version.txt` and `/firmware-app-<board>.bin` URLs permanently
serve the small **v1.35 bridge**. A device still on an older version therefore
installs v1.35 first, rather than attempting a future image that will not fit.
On C3/C6, complete **Prepare device** and check for updates again.

Firmware v1.35 and later uses `/updates/ota-version.txt` and
`/updates/firmware-app-<board>.bin` for the latest release. The USB manifest and
explicit versioned downloads also use the current release. The bridge files and
their checksums are retained unchanged; later releases must not replace them.

Do not upload `firmware-<board>.bin` or `firmware.bin` on the OTA page. Those are USB merged images.
For OTA, use `firmware-app-<board>.bin`; `firmware-app.bin` is the compatible C6 alias.

## Build and host

Use the [source build instructions](../esp32-birdnet-mic/README.md#build-and-flash-manually).
Keep offsets from the target board's generated `flash_args`; do not reuse another chip's offsets.

Host `index.html`, `manifest.json`, the firmware parts and `ota-version.txt` together.
WebSerial needs HTTPS (or localhost). JSON and firmware must have the appropriate MIME types:
`application/json` and `application/octet-stream`. Automatic device OTA additionally needs direct
HTTP access to the board-specific app files without redirects.

GitHub release assets include all four boards' bootloader, partition, boot_app0, app-only and merged
images, plus the manifest and version file. See the [changelog](../esp32-birdnet-mic/CHANGELOG.md).
