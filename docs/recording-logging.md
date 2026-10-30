# Recording logs

In **Recording → Recording log**, choose **Normal** for everyday operation.
Normal logging records recording starts, saved WAV files and sizes, stops,
thermal pauses/resumes and errors. **Detailed diagnostics** additionally records
card/SPI details, individual preparation steps and progress every five minutes.
Normal is the default.

## Persistent microSD log

**Append the device log to microSD in Recording mode** is enabled by default.
The device appends recording events and queued device messages to
`/recording.log`; existing contents survive recording sessions and restarts.
Streaming mode does not mount or write the SD card for logging.

The default size limit is **1024 KiB**. At the limit, the file becomes
`/recording.log.1` and a new `/recording.log` begins. There is one previous log;
its older predecessor is replaced at the next rotation. The setting is
configurable from **64 to 16384 KiB** per file. WAV recordings are preserved.
Turning SD logging off preserves existing log files.

The SD worker owns both WAV and log I/O. Queued messages are flushed during
recording checkpoints (approximately every five seconds), at rotation and before
Stop & save completes. Log write failures have their own warning/status; audio
continues whenever the card still accepts WAV writes. Log history cannot be
written to an absent or failed card.

**Read recent SD log** shows the last 2 KiB of the current file after
Stop & save. **Read previous SD log** shows the previous file. The full files
are available by reading the microSD card on a computer. Reading a missing
previous log reports that no previous file exists yet.

Time stamps use the device's selected local time when available, otherwise
uptime. Changing operating modes, firmware settings or the logging level is
available after Stop & save.

## Bounded recent log and memory

The ordinary **Logs** panel has an automatic, fixed byte budget:

| Available memory | Streaming | Recording |
| --- | ---: | ---: |
| PSRAM allocated successfully | 32 KiB in PSRAM | 32 KiB in PSRAM |
| Internal RAM only | 8 KiB | 4 KiB |

On boards without PSRAM, switching to Recording shrinks the buffer and keeps
its newest complete messages. Failed growth safely retains the smaller buffer;
the UI reports the actual capacity. The buffer never grows with message count.
The recording PCM queue/network reserve is preserved.

**Older**, **Newer** and **Latest** browse the retained history in pages of at
most 2 KiB. Automatic refresh pauses while reading an older page. **Download
Logs** exports the complete retained RAM log using bounded response chunks.
The RAM history size is separate from the microSD file-size limit.

A burst larger than a pending queue can discard older messages; a buffer-loss
notice is recorded when possible. Save/download the on-card log when a permanent
history matters. The recent UI log itself resets with the device.

## API

All POST requests use `X-ESP32MIC-CSRF: 1` and require an idle recorder.

- `POST /api/recording/log/settings`: `sd_enabled=0|1`, `detailed=0|1`,
  `max_kib=64..16384`. Settings have their own persistent namespace.
- `GET /api/status`: `recording.logging` provides settings, file name, current
  byte count and a separate log error string.
- `POST /api/recording/log/read`: `archive=0|1` schedules a bounded read in the
  SD worker and preserves the last WAV result.
- `GET /api/recording/log`: HTTP 202 while waiting; HTTP 200 with recent text
  and `X-ESP32MIC-Log-Bytes` for the complete file size; an error if unavailable.

The paged recent log is also available at `GET /api/logs?page=1`, optionally
with `before=<cursor>` or `after=<cursor>`. Response headers provide the oldest,
latest and page cursor boundaries, actual capacity and PSRAM/internal storage.
Invalid or overflowing cursors are rejected; overwritten history is clamped to
available messages and reported. Legacy `GET /api/logs` and `?download=1` still
return the complete retained history without allocating it as one large String.

## Recording with Wi-Fi off

After Wi-Fi is turned off, a completed recording or recording error puts the
device into deep sleep once the storage worker has closed the files and log.
A thermal pause with pending recovery keeps its cooling/resume behavior.
Restart or power-cycle the device to restore Wi-Fi; normal sleep has no wake timer.
With Wi-Fi on, finishing a recording leaves the controls available.

Enabled low-battery protection remains active throughout Recording mode,
including before a session and after a saved recording or SD error. It follows
the existing low-battery sleep setting.

If the radio cannot be turned off, firmware restores the control connection,
reports the error and permits retry. The UI also restores controls if the device
becomes reachable after an interrupted connection.
