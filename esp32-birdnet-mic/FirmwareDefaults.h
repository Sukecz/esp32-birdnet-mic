#pragma once
// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 1.5f
#define DEFAULT_BUFFER_SIZE 512    // Balanced default; safer for BirdNET-Pi UDP than 1024-sample packets
#define DEFAULT_WIFI_TX_DBM 15.0f  // Stable default; higher power adds substantial heat on C6
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_PUBLISH_INTERVAL_SEC 60
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 500

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5
