#pragma once
#include <Arduino.h>

// Web UI (ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi): initialization and request handling
void webui_begin();
bool webui_logModeChanged(bool recordingMode);
void webui_handleClient();

// Push a log line from main into the Web UI ring buffer
void webui_pushLog(const String &line);
void webui_pushRecordingLog(const String &line);
