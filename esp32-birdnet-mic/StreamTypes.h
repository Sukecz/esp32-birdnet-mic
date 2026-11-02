#pragma once
#include <stdint.h>

enum StreamTarget : uint8_t {
    STREAM_TARGET_BIRDNET_GO = 0,
    STREAM_TARGET_BIRDNET_PI = 1
};

struct StreamProfileConfig {
    uint8_t target = STREAM_TARGET_BIRDNET_GO;
};

struct StreamStats {
    uint8_t clientCount = 0;
    bool streaming = false;
    uint32_t packetsSent = 0;
    unsigned long statsResetMs = 0;
    unsigned long lastConnectMs = 0;
    unsigned long lastPlayMs = 0;
};

enum MicFormat : uint8_t {
    MIC_FORMAT_PHILIPS = 0,  // ICS-43434 / INMP441
    MIC_FORMAT_MSB = 1,      // Adafruit SPH0645LM4H
    MIC_FORMAT_PDM = 2       // IM73D122, 48 kHz PCM
};

static constexpr uint8_t DEFAULT_MIC_FORMAT = MIC_FORMAT_PHILIPS;

constexpr uint8_t MAX_CLIENTS = 3;
constexpr uint32_t PDM_SAMPLE_RATE = 48000;
