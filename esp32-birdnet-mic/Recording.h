#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace recording {
// Shared SD protocol and clock across the supported XIAO family. Buffer
// allocation adapts to available PSRAM/internal RAM rather than the chip name.
constexpr uint32_t kDefaultStorageClockHz = 16000000, kDefaultWriteBlockBytes = 4096;
enum class State : uint8_t { Ready, Starting, Recording, Saving, Saved, Error, Checking, Cooling };
struct Settings {
    uint32_t magic = 0x52454331;
    uint32_t durationMinutes = 0; // 0 = continuous, max 7 days
    uint16_t segmentMinutes = 60;
    uint8_t recordingMode = 0;
    uint8_t autoStart = 0;
};
struct Status {
    State state = State::Ready;
    uint32_t logBytes = 0;
    char logError[80] = {};
    uint32_t elapsedSeconds = 0;
    uint32_t droppedSamples = 0;
    uint32_t writtenKiB = 0;
    uint32_t freeMiB = 0;
    uint64_t freeBytes = 0;
    uint32_t maxWriteMs = 0;
    bool cardReady = false;
    uint32_t spiHz = kDefaultStorageClockHz, writeBlockBytes = kDefaultWriteBlockBytes, cardMiB = 0, volumeMiB = 0, clusterBytes = 0;
    uint8_t cardType = 0, fatBits = 0;
    uint32_t sdDataToken = 0, sdCommand = 0, sdCommandReply = 0, sdStatus = 0, sdWaitTimeouts = 0;
    int ioErrno = 0;
    uint32_t ioOffset = 0, ioRequested = 0;
    int32_t ioReturned = 0;
    char ioOperation[12] = {};
    bool diagnostic = false, verified = false;
    uint32_t verifiedBytes = 0;
    uint32_t diagnosticWriteMs = 0, diagnosticReadMs = 0, queueBytes = 0;
    char file[96] = {};
    char error[80] = {};
};
struct LogSettings {
    uint32_t magic = 0x4c4f4731;
    uint32_t maxKiB = 977; // Approximately 1 decimal MB.
    uint8_t sdEnabled = 1, detailed = 0, reserved[2] = {};
};
LogSettings logSettings();
bool saveLogSettings(const LogSettings& settings);
void queueSystemLog(const char* line);
bool requestLogTail(bool archive, bool hardwareSupported, uint8_t boardVariant = 0);
bool logTailBusy();
bool logTailReady();
String logTail();
String logTailError();
uint32_t logTailFileBytes();
void begin();
Settings settings();
bool saveSettings(const Settings& value);
bool mode();
void setRuntimeMode(bool enabled); // Startup choice is owned by DeviceSetup.
bool busy();
bool resumePending();
bool pauseForCooling();
void cancelResume();
bool resume(uint32_t rate, bool hardwareSupported, uint8_t boardVariant = 0);
bool start(uint32_t rate, bool hardwareSupported, uint8_t boardVariant = 0);
bool checkCard(bool hardwareSupported, uint8_t boardVariant = 0); // async, no recording files created
// Runtime-only clock selection for controlled storage diagnostics; no NVS write.
bool setStorageClock(uint32_t hz, uint32_t writeBlockBytes = kDefaultWriteBlockBytes);
bool diagnose(bool hardwareSupported, uint8_t boardVariant = 0);
void push(const int16_t* pcm, size_t samples); // capture only; never waits for SD
enum class StopReason : uint8_t { User, LowBattery, Thermal, ModeChange, Update };
void requestStop(StopReason reason = StopReason::User);
void captureFault();
bool stop(uint32_t timeoutMs = 10000); // false: caller must defer destructive action
Status status();
// Bounded worker events; main loop and SD worker each consume their own copy.
bool nextLog(String& line);
String statusJson(bool hardwareSupported, uint8_t boardVariant = 0, uint32_t sampleRate = 48000);
}
