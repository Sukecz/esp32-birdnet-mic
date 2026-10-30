# Time synchronization and post-OTA checks

Firmware 2.00 starts NTP asynchronously when Wi-Fi becomes
available, including the first connection after boot. It retries unanswered
requests after 30 seconds, limits manual/reconnect requests to at most one per
15 seconds, and uses a six-hour interval after success. No network response is
awaited in the audio/application loop.

**Sync Status** changes to **YES** only after the SNTP notification callback
confirms a valid server response. The callback queues an atomic event; the main
loop updates the status and Last Sync. A retained RTC time alone is not counted
as a new synchronization. Late/background responses update Last Sync as well.
Changing the timezone applies the new local offset without restarting NTP.

The UI shows **Waiting for Wi-Fi**, **Waiting for NTP**, **YES** or **OFF**.
An already-valid clock remains visible while new synchronization is pending or
Time Sync is off. **Sync Time Now** requests synchronization; it does not claim
that the server has already replied. The button is disabled while Time Sync is off.
Cold-start standalone Recording without Internet can still have an unavailable
clock; WAV files retain the existing `unsynced` naming fallback in that case.

`/api/status` reports `time_valid` separately from `time_synced`, plus
`time_sync_status`, `last_time_sync`, UTC, local time and the effective offset.

## Regression coverage

- `tests/test_time_sync.py` compiles the actual SNTP callback, configuration,
  request and scheduler functions. Scenarios include an offline first attempt,
  Wi-Fi already connected at the first loop check, delayed replies, retained RTC,
  a background resync, unanswered requests, reconnect rate limiting, disabled /
  re-enabled NTP, timezone changes, invalid replies and `millis()` wraparound.
  Blocking `getLocalTime()` / delay calls are rejected by the harness.
- `tests/time_sync_browser_test.js` loads source and embedded/minified UI assets,
  exercises waiting/valid/synchronized/offline/disabled states, and checks manual
  request feedback. It covers mobile/desktop and light/dark themes.
- `tests/test_device_status_check.py` verifies that the post-OTA check rejects
  `NO / never` even with an accurate retained RTC, stale UTC, an incorrect local
  offset and unhealthy capture. Offline/direct and explicitly disabled NTP are
  handled separately.

These tests do not replace a live network test. After a configured-device OTA,
check settings preservation, served assets and the relevant UI, then run:

```bash
python3 tools/check_device_status.py http://DEVICE_IP --board xiao-esp32c6 --wait 60
```

The checker only reads device APIs. With Time Sync on and Wi-Fi connected, it
requires actual synchronization, a recorded Last Sync, UTC within five seconds
of the checking host and local time matching the selected offset. It also checks
microphone capture, I2S errors and available DMA-overrun counters. It prints only
a short health result; passwords, SSIDs and MQTT configuration are not printed.

For an evidence file, add `--output PATH.json`. A host clock synchronized to a
reliable source is required to assess device UTC accuracy.

SNTP callback behavior and thread-safe API references:
[Espressif system-time documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32c6/api-reference/system/system_time.html).
