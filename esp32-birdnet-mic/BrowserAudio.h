#pragma once
#include <stddef.h>
#include <stdint.h>

struct BrowserAudioDiagnostics {
    const char* lastStop = "none";
    uint32_t stops = 0, overflows = 0, sendTimeouts = 0, noAudioTimeouts = 0;
    uint32_t socketErrors = 0, maxLoopGapMs = 0, bytesSent = 0;
    bool independentWorker = false;
    uint32_t workerStackFreeBytes = 0;
    uint32_t previewDiscardedBytes = 0;
    uint32_t maxSendStallMs = 0, sendRecoveries = 0;
};
BrowserAudioDiagnostics browserAudioDiagnostics();
void browserAudioBegin();
size_t browserAudioQueueBytes();
void browserAudioLoop(bool allowed, uint32_t sampleRate);
void browserAudioStop();
bool browserAudioActive();
void browserAudioPush(const int16_t* samples, size_t count);
