# Source map and change boundaries

This describes the current layout. The incremental migration plan is in
[development.md](development.md#incremental-architecture-plan).

| Area | Entry points / files | Ownership and constraints |
| --- | --- | --- |
| Startup and orchestration | `esp32-birdnet-mic.ino`, `setup()`, `loop()` | Currently also owns audio settings, capture lifecycle, RTSP and MQTT integration. Avoid adding unrelated feature state here. |
| Device setup | `DeviceSetup.cpp`, `SetupConfig.h` | Boot mode, network/setup choices and persistence. A temporary runtime mode switch is distinct from a saved startup preference. |
| Audio capture and processing | `.ino`, `PdmCapture.h`, `PdmFilter.h`, `FixedAudio.h`, `MicrophoneProfiles.h` | Capture/DSP working buffers and DMA use internal RAM. PSRAM-capable storage is selected separately. |
| Audio storage policy | `AudioMemory.h`, `AudioReserve.h`, `AudioDiagnostics.h` | Physical chip capacity does not replace capability-aware allocation, reserves or failure handling. |
| Browser Listen | `BrowserAudio.cpp`, `PreviewBuffer.h` | Independent consumer in Streaming and Recording; bounded queues and socket work must not block capture. |
| SD recording | `Recording.cpp`, `RecordingWav.h`, `SdJournal.h`, `RecordingMemory.h` | Recording state, SD worker, PCM queue and recovery. `push()` never waits for SD; destructive changes must respect incomplete stop. |
| Power and thermal | `Battery.cpp`, `FieldPower.cpp`, `ThermalRecovery.h`, `PowerPolicy.h` | Temporary thermal pause/recovery differs from final recording completion or Wi-Fi shutdown. |
| Network helpers | `WifiAp.cpp`, `DiscoveryClient.h`, `CooperativeTcpWriter.h`, `MqttDiscovery.h` | Keep reconnect/backpressure bounded and preserve capture responsiveness. |
| HTTP API and UI serving | `WebUI.cpp`, `WebUI.h` | API validation/authentication and serving assets. Future handlers should delegate to module interfaces. |
| Browser UI source | `webui/`, `tools/gen_webui_gzip_header.sh` under sketch | Source files generate embedded headers; both forms require tests. Drafts survive polling; saves affect only their logical group. |
| OTA layout | `FlashLayout.cpp`, `FlashLayoutTable.h`, `tools/partitions/` | Preserve upgrade compatibility and immutable bridge assets. Flash capacity is distinct from active device partition layout. |
| Targets and automation | Root `boards.json`, `tools/board_registry.py`, `tools/build.py`, `tools/test.py` | One supported-board registry; development, packaging and publishing are distinct operations. |

For a new feature, identify its owner, persistent settings, transitions, failure
behavior and allocations first. Add tests at that owner's interface. When an
existing owner is too tightly coupled to `.ino`, extract its current behavior in
a separate change before extending it. Prefer explicit small configuration/status
structures over additional cross-module globals. Introduce no general event bus,
service locator or dynamic dispatch unless a concrete need justifies its cost.

During extraction preserve settings keys/defaults, API responses, mode transitions,
buffer ownership and OTA paths. Keep pure decision logic independent of Arduino
where practical so host tests compile production code directly. Hardware adapters
remain responsible for timing, capabilities and I/O failure. A mock passing is
evidence for the modeled behavior, not SD/audio/radio hardware qualification.
