#pragma once
#include <Arduino.h>
// User choices are independent from the effective GPIO/format configuration.
// Existing NVS selections migrate to Manual; fresh devices start with Auto.
extern bool micAutoDetect, s3BoardAutoDetect, microphoneGainConfigured;
extern String microphoneDetectionNote;
extern int8_t microphoneDetectedFormat;
extern int8_t detectedS3Board;
void detectMicrophoneAtBoot();
void confirmMicrophoneBoot();
bool applyMicrophoneChoice(uint8_t format, bool recommended = false);
bool applyBoardChoice(uint8_t variant);
