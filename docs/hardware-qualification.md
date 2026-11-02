# Hardware qualification

Build coverage and hardware qualification are separate. At this work stage only
the configured C6 assembly is available. Do not infer C3/S3/C5 operation, SD
reliability or battery runtime from a successful compile or a C6 observation.

| Target | Required build checks | Physical state |
| --- | --- | --- |
| C3 | Standard target, app/partition/ELF budgets | Unavailable; qualification pending |
| S3 | PSRAM and disabled-PSRAM builds, memory regressions | Unavailable; qualification pending |
| C5 | PSRAM and disabled-PSRAM builds, memory regressions | Unavailable; SD/battery/PDM qualification pending |
| C6 | Standard target and budgets | Available; qualify the observed Streaming/RTSP/Listen workload |

Run a read-only observation from the repository root:

```sh
python3 tools/qualify_runtime.py http://DEVICE --board c6 --seconds 600 --listen --output tmp/qualification/c6.json
```

Use longer durations (for example 86,400 seconds) for a day-long soak. The tool
verifies identity before opening Listen, drains audio without storing PCM, reads
capture/time/memory status, and saves only selected non-secret telemetry. It never
changes settings, modes, Wi-Fi, reboot state or firmware. Listen temporarily uses
the preview consumer; do not run it over another person's active Listen session.

Reports include observed minima for free/minimum/largest internal heap and free
PSRAM, capture failures and counter growth. Existing API memory counters are
integer KiB, so converted byte values have 1,024-byte resolution. Physical SRAM
includes instruction/data regions; it is not free heap. The historical minimum
heap can include startup or activity before this observation. A passing report
applies to that board, firmware, duration and workload only.

No new universal free-heap threshold is guessed from physical RAM. Existing
allocation reserves in `AudioReserve.h` and `RecordingMemory.h` remain enforced.
Before approving a tighter future budget, retain observations across cold boot,
capture restart, RTSP backpressure, Listen reconnect, settings/UI polling, and
hardware-specific recording/power workloads. Include worst observed free memory,
largest contiguous block, failures, duration and remaining allocation headroom.

Use `tools/check_live_ui.cjs` with `NODE_PATH=tools/node_modules` and `CHROMIUM`
to inspect the real device in 320/390/1280 px and both themes. It checks orange
drafts, polling preservation, revert/discard and overflow; all non-GET requests
are blocked. Mock browser tests cover saving/error paths without writing device
settings. C6's external antenna must remain selected.
