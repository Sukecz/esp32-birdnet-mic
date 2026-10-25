#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace recording {
enum class State : uint8_t { Ready, Starting, Recording, Saving, Saved, Error };
struct Settings {
    uint32_t magic = 0x52454331;
    uint32_t durationMinutes = 0; // 0 = continuous, max 7 days
    uint16_t segmentMinutes = 60;
    uint8_t recordingMode = 0;
    uint8_t autoStart = 0;
};
struct Status {
    State state = State::Ready;
    uint32_t elapsedSeconds = 0;
    uint32_t droppedSamples = 0;
    uint32_t writtenKiB = 0;
    uint32_t freeMiB = 0;
    uint32_t maxWriteMs = 0;
    bool cardReady = false;
    char file[96] = {};
    char error[80] = {};
};
void begin();
Settings settings();
bool saveSettings(const Settings& value);
bool mode();
bool busy();
bool start(uint32_t rate, bool hardwareSupported);
void push(const int16_t* pcm, size_t samples); // capture only; never waits for SD
void requestStop();
void captureFault();
bool stop(uint32_t timeoutMs = 10000); // false: caller must defer destructive action
Status status();
String statusJson(bool hardwareSupported);
}
