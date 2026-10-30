# Manual OTA firmware

> **Firmware 2.00:** Streaming and standalone Recording. See the [main guide](../README.md).


This folder contains app-only firmware files for manual OTA update through the device
Web UI.

Open the device update page:

```text
http://<device-ip>/ota
```

Choose **Upload compiled file** and select the file matching your board:

```text
firmware-app-c3.bin    XIAO ESP32-C3
firmware-app-s3.bin    XIAO ESP32-S3
firmware-app-c5.bin    XIAO ESP32-C5
firmware-app-c6.bin    XIAO ESP32-C6
```

`firmware-app.bin` is kept as a compatibility alias for the XIAO ESP32-C6 build.

Upload only the file for your board. Do not use merged `firmware-<board>.bin` images for OTA.
Saved settings and the partition layout are retained during a normal update.

**C3/C6 preparation:** the first update from older firmware to v1.35 works
normally. After reboot, open **Firmware update**, click **Prepare device** and
confirm. Keep power connected and wait for **Preparation complete**. Your
settings are kept; restarts and reconnection happen automatically.

The new firmware requires this step before another manual or automatic OTA.
If you postpone it, the microphone keeps working. S3/C5 and devices already
using the larger layout skip preparation. A power failure during preparation
can require USB recovery. Older firmware always receives v1.35 first through automatic OTA before moving
to the latest release.

PDM (IM73D122 / Adafruit PDM / MP34DT01) support is **🟡 Experimental**. On C3/C5 use 512 samples per packet and 160 MHz;
keeping saved values preserves the packet size; **Recommended settings** applies 512.
See the [wiring and setup guide](../esp32-birdnet-mic/README.md#pdm-microphones).
