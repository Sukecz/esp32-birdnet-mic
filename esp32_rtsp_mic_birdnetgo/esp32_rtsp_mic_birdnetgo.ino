#include <WiFi.h>
#include <WiFiManager.h>
#include "driver/i2s_std.h"   // new I2S driver (legacy driver/i2s.h was removed in Arduino-ESP32 4.x)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <math.h>
#include "WebUI.h"

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "1.4.0"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;

// OTA password (optional):
// - For production, set a strong password to protect OTA updates.
// - You can leave it undefined to disable password protection.
// - Example placeholder (edit as needed):
// #define OTA_PASSWORD "1234"  // Optional: change or leave undefined

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 1.2f
#define DEFAULT_BUFFER_SIZE 1024   // Stable streaming profile by default
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 500

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5

// -- Pins
#define I2S_BCLK_PIN    21
#define I2S_LRCLK_PIN   1
#define I2S_DOUT_PIN    2

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

// -- RTSP Streaming
String rtspSessionId = "";
volatile bool isStreaming = false;
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
uint8_t rtspParseBuffer[1024];
int rtspParseBufferPos = 0;

// -- Audio pipeline (v1.4.0)
// A dedicated FreeRTOS task reads the microphone in small fixed chunks and pushes
// processed 16-bit samples into a ring buffer. The main loop (RTSP, Web UI, OTA)
// only pulls whole packets out of that ring, so slow Wi-Fi writes or Web UI
// requests can no longer starve the I2S DMA and silently drop audio.
#define CAPTURE_CHUNK_SAMPLES 256          // ~5.3 ms at 48 kHz
#define AUDIO_RING_BYTES      (64 * 1024)  // ~680 ms of 16-bit audio at 48 kHz
#define CAPTURE_TASK_PRIORITY 10           // above loop() (1), below Wi-Fi/lwIP
#define CAPTURE_TASK_STACK    4096
#define SEND_TIMEOUT_MS       2000         // give up on a stuck TCP write after this

static int32_t captureRaw[CAPTURE_CHUNK_SAMPLES];
static int16_t captureOut[CAPTURE_CHUNK_SAMPLES];
static StreamBufferHandle_t audioRing = nullptr;
// Pause handshake guarding the I2S driver + DSP state. (A mutex does not work here:
// the higher-priority capture task would immediately re-take it and starve loop().)
static volatile bool capturePauseReq = false;
static volatile bool capturePaused = false;
static i2s_chan_handle_t i2sRxChan = nullptr;
static TaskHandle_t captureTaskHandle = nullptr;
static volatile bool i2sReady = false;
uint8_t* rtpPacketBuffer = nullptr;              // 4 (interleave) + 12 (RTP) + payload
size_t rtpPacketBufferSize = 0;

// -- Pipeline diagnostics (reset on each PLAY)
volatile uint32_t i2sOverflowCount = 0;   // I2S DMA overruns (capture task too slow)
volatile uint32_t ringDroppedSamples = 0; // samples dropped because sender fell >ring behind
uint32_t sendFailCount = 0;               // connections closed due to stuck TCP writes (since boot)
uint8_t ringPeakFillPct = 0;              // highest ring fill seen while streaming

// -- Test tone (diagnostic, not persisted): replaces mic samples with a clean sine
volatile bool testToneEnabled = false;
volatile uint16_t testToneHz = 1000;
static int16_t sineTable[256];
static uint32_t tonePhase = 0;

// Called from loop() context only. Blocks until the capture task is parked outside
// any I2S/DSP access (at most one ~5 ms read), then lets the caller touch that state.
static void audioPause() {
    if (!captureTaskHandle) return;          // task not started yet: nothing to wait for
    capturePauseReq = true;
    while (!capturePaused) vTaskDelay(1);
}
static void audioResume() {
    capturePauseReq = false;
}

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
uint8_t i2sShiftBits = 12;  // (1) compile-time default respected on first boot

// -- Audio metering / clipping diagnostics
uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
uint32_t audioClipCount = 0;      // total blocks where clipping occurred
bool audioClippedLastBlock = false; // clipping occurred in last processed block
uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldUntilMs = 0; // when to clear hold

// -- High-pass filter (biquad) to cut low-frequency rumble
struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    inline void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
Biquad hpf;
uint32_t hpfConfigSampleRate = 0;
uint16_t hpfConfigCutoff = 0;
bool hpfConfigEnabled = false;

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics, auto-recovery and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastPerformanceCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastTempCheck = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = true;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
float overheatTripTemp = 0.0f;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason = "";
String overheatLastTimestamp = "";
bool overheatSensorFault = false;
bool overheatLatched = false;

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum acceptable packet rate (restart below this)
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = 160;          // CPU frequency (default 160 MHz)

// -- WiFi TX power (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;

// ===============================================

// Helper: convert WiFi power enum to dBm (for logs)
float wifiPowerLevelToDbm(wifi_power_t lvl) {
    switch (lvl) {
        case WIFI_POWER_19_5dBm:    return 19.5f;
        case WIFI_POWER_19dBm:      return 19.0f;
        case WIFI_POWER_18_5dBm:    return 18.5f;
        case WIFI_POWER_17dBm:      return 17.0f;
        case WIFI_POWER_15dBm:      return 15.0f;
        case WIFI_POWER_13dBm:      return 13.0f;
        case WIFI_POWER_11dBm:      return 11.0f;
        case WIFI_POWER_8_5dBm:     return 8.5f;
        case WIFI_POWER_7dBm:       return 7.0f;
        case WIFI_POWER_5dBm:       return 5.0f;
        case WIFI_POWER_2dBm:       return 2.0f;
        case WIFI_POWER_MINUS_1dBm: return -1.0f;
        default:                    return 19.5f;
    }
}

// Helper: pick the highest power level not exceeding requested dBm
static wifi_power_t pickWifiPowerLevel(float dbm) {
    if (dbm <= -1.0f) return WIFI_POWER_MINUS_1dBm;
    if (dbm <= 2.0f)  return WIFI_POWER_2dBm;
    if (dbm <= 5.0f)  return WIFI_POWER_5dBm;
    if (dbm <= 7.0f)  return WIFI_POWER_7dBm;
    if (dbm <= 8.5f)  return WIFI_POWER_8_5dBm;
    if (dbm <= 11.0f) return WIFI_POWER_11dBm;
    if (dbm <= 13.0f) return WIFI_POWER_13dBm;
    if (dbm <= 15.0f) return WIFI_POWER_15dBm;
    if (dbm <= 17.0f) return WIFI_POWER_17dBm;
    if (dbm <= 18.5f) return WIFI_POWER_18_5dBm;
    if (dbm <= 19.0f) return WIFI_POWER_19dBm;
    return WIFI_POWER_19_5dBm;
}

// Apply WiFi TX power
// Logs only when changed; can be muted with log=false
void applyWifiTxPower(bool log = true) {
    wifi_power_t desired = pickWifiPowerLevel(wifiTxPowerDbm);
    if (desired != currentWifiPowerLevel) {
        WiFi.setTxPower(desired);
        currentWifiPowerLevel = desired;
        if (log) {
            simplePrintln("WiFi TX power set to " + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + " dBm");
        }
    }
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass).
// Caller must have paused the capture task (audioPause) or be the capture task so the filter state is not
// modified while samples are being processed.
static void computeHighpassCoeffsLocked() {
    hpfConfigEnabled = highpassEnabled;
    if (!highpassEnabled) {
        hpf.reset();
        hpfConfigSampleRate = currentSampleRate;
        hpfConfigCutoff = highpassCutoffHz;
        return;
    }
    float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;
    if (fc < 10.0f) fc = 10.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f; // keep reasonable

    const float pi = 3.14159265358979323846f;
    float w0 = 2.0f * pi * (fc / fs);
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q = 0.70710678f; // Butterworth-like
    float alpha = sinw0 / (2.0f * Q);

    float b0 =  (1.0f + cosw0) * 0.5f;
    float b1 = -(1.0f + cosw0);
    float b2 =  (1.0f + cosw0) * 0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    hpf.b0 = b0 / a0;
    hpf.b1 = b1 / a0;
    hpf.b2 = b2 / a0;
    hpf.a1 = a1 / a0;
    hpf.a2 = a2 / a0;
    hpf.reset();

    hpfConfigSampleRate = currentSampleRate;
    hpfConfigCutoff = highpassCutoffHz;  // store the requested value so change detection is stable
}

// Thread-safe wrapper used by setup and the Web UI.
void updateHighpassCoeffs() {
    audioPause();
    computeHighpassCoeffsLocked();
    audioResume();
}

// Uptime -> "Xd Yh Zm Ts"
String formatUptime(unsigned long seconds) {
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;

    String result = "";
    if (days > 0) result += String(days) + "d ";
    if (hours > 0 || days > 0) result += String(hours) + "h ";
    if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
    result += String(seconds) + "s";
    return result;
}

// Format "X ago" for events based on millis()
String formatSince(unsigned long eventMs) {
    if (eventMs == 0) return String("never");
    unsigned long seconds = (millis() - eventMs) / 1000;
    return formatUptime(seconds) + " ago";
}

static bool isTemperatureValid(float temp) {
    if (isnan(temp) || isinf(temp)) return false;
    if (temp < -20.0f || temp > 130.0f) return false;
    return true;
}

// Format current local time, fallback to uptime when no RTC/NTP time available
static void persistOverheatNote() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();
}

void recordOverheatTrip(float temp) {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    overheatTripTemp = temp;
    overheatTriggeredAt = millis();
    overheatLastTimestamp = formatUptime(uptimeSeconds);
    overheatLastReason = String("Thermal shutdown: ") + String(temp, 1) + " C reached (limit " +
                         String(overheatShutdownC, 1) + " C). Stream disabled; acknowledge in UI.";
    overheatLatched = true;
    simplePrintln("THERMAL PROTECTION: " + overheatLastReason);
    simplePrintln("TIP: Improve cooling or lower WiFi TX power/CPU MHz if overheating persists.");
    persistOverheatNote();
}

// Temperature monitoring + thermal protection
void checkTemperature() {
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    bool tempValid = isTemperatureValid(temp);
    if (!tempValid) {
        lastTemperatureValid = false;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
            overheatLastTimestamp = "";
            overheatTripTemp = 0.0f;
            overheatTriggeredAt = 0;
            persistOverheatNote();
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal protection paused.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;

    if (overheatSensorFault) {
        overheatSensorFault = false;
        overheatLastReason = "Thermal protection restored: temperature sensor reading valid.";
        overheatLastTimestamp = formatUptime((millis() - bootTime) / 1000);
        persistOverheatNote();
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }

    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    bool protectionActive = overheatProtectionEnabled && !overheatSensorFault;
    if (protectionActive) {
        if (!overheatLockoutActive && temp >= overheatShutdownC) {
            overheatLockoutActive = true;
            recordOverheatTrip(temp);
            // Disable streaming until user restarts manually
            if (rtspClient && rtspClient.connected()) {
                rtspClient.stop();
            }
            if (isStreaming) {
                isStreaming = false;
            }
            rtspServerEnabled = false;
            rtspServer.stop();
        } else if (overheatLockoutActive && temp <= (overheatShutdownC - OVERHEAT_LIMIT_STEP_C)) {
            // Allow re-arming after we cool down by at least one step
            overheatLockoutActive = false;
        }
    } else {
        overheatLockoutActive = false;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    float warnThreshold = max(overheatShutdownC - 5.0f, (float)OVERHEAT_MIN_LIMIT_C);
    if (temp > warnThreshold && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected (" + String(temp, 1) + " C). Approaching shutdown limit.");
        lastTempWarn = millis();
    }
}

// Performance diagnostics
void checkPerformance() {
    uint32_t currentHeap = ESP.getFreeHeap();
    if (currentHeap < minFreeHeap) {
        minFreeHeap = currentHeap;
    }

    if (isStreaming && (millis() - lastStatsReset) > 30000) {
        uint32_t runtime = millis() - lastStatsReset;
        uint32_t currentRate = (audioPacketsSent * 1000) / runtime;

        if (currentRate > maxPacketRate) maxPacketRate = currentRate;
        if (currentRate < minPacketRate) minPacketRate = currentRate;

        if (currentRate < minAcceptableRate) {
            simplePrintln("PERFORMANCE DEGRADATION DETECTED!");
            simplePrintln("Rate " + String(currentRate) + " < minimum " + String(minAcceptableRate) + " pkt/s");

            if (autoRecoveryEnabled) {
                simplePrintln("AUTO-RECOVERY: Restarting I2S...");
                restartI2S();
                audioPacketsSent = 0;
                lastStatsReset = millis();
                lastI2SReset = millis();
            }
        }
    }
}

// WiFi health check
void checkWiFiHealth() {
    if (WiFi.status() != WL_CONNECTED) {
        simplePrintln("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
    }

    // Re-apply TX power WITHOUT logging (prevent periodic log spam)
    applyWifiTxPower(false);

    int32_t rssi = WiFi.RSSI();
    if (rssi < -85) {
        simplePrintln("WARNING: Weak WiFi signal: " + String(rssi) + " dBm");
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled) return;

    unsigned long uptimeHours = (millis() - bootTime) / 3600000;
    if (uptimeHours >= resetIntervalHours) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        delay(1000);
        ESP.restart();
    }
}

// Load settings from flash
void loadAudioSettings() {
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // (1) respect compile-time default 12 on first boot
    i2sShiftBits = audioPrefs.getUChar("shiftBits", i2sShiftBits);
    autoRecoveryEnabled = audioPrefs.getBool("autoRecovery", true);
    scheduledResetEnabled = audioPrefs.getBool("schedReset", false);
    resetIntervalHours = audioPrefs.getUInt("resetHours", 24);
    minAcceptableRate = audioPrefs.getUInt("minRate", 50);
    performanceCheckInterval = audioPrefs.getUInt("checkInterval", 15);
    autoThresholdEnabled = audioPrefs.getBool("thrAuto", true);
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", 160);
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    highpassEnabled = audioPrefs.getBool("hpEnable", DEFAULT_HPF_ENABLED);
    highpassCutoffHz = (uint16_t)audioPrefs.getUInt("hpCutoff", DEFAULT_HPF_CUTOFF_HZ);
    overheatProtectionEnabled = audioPrefs.getBool("ohEnable", DEFAULT_OVERHEAT_PROTECTION);
    uint32_t ohLimit = audioPrefs.getUInt("ohThresh", DEFAULT_OVERHEAT_LIMIT_C);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    ohLimit = OVERHEAT_MIN_LIMIT_C + ((ohLimit - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C) * OVERHEAT_LIMIT_STEP_C;
    overheatShutdownC = (float)ohLimit;
    overheatLastReason = audioPrefs.getString("ohReason", "");
    overheatLastTimestamp = audioPrefs.getString("ohStamp", "");
    overheatTripTemp = audioPrefs.getFloat("ohTripC", 0.0f);
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    audioPrefs.end();

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (overheatLatched) {
        rtspServerEnabled = false;
    }
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
    simplePrintln("Loaded settings: Rate=" + String(currentSampleRate) +
                  ", Gain=" + String(currentGainFactor, 1) +
                  ", Buffer=" + String(currentBufferSize) +
                  ", WiFiTX=" + String(txShown, 1) + "dBm" +
                  ", shiftBits=" + String(i2sShiftBits) +
                  ", HPF=" + String(highpassEnabled?"on":"off") +
                  ", HPFcut=" + String(highpassCutoffHz) + "Hz");
}

// Save settings to flash
void saveAudioSettings() {
    audioPrefs.begin("audio", false);
    audioPrefs.putUInt("sampleRate", currentSampleRate);
    audioPrefs.putFloat("gainFactor", currentGainFactor);
    audioPrefs.putUShort("bufferSize", currentBufferSize);
    audioPrefs.putUChar("shiftBits", i2sShiftBits);
    audioPrefs.putBool("autoRecovery", autoRecoveryEnabled);
    audioPrefs.putBool("schedReset", scheduledResetEnabled);
    audioPrefs.putUInt("resetHours", resetIntervalHours);
    audioPrefs.putUInt("minRate", minAcceptableRate);
    audioPrefs.putUInt("checkInterval", performanceCheckInterval);
    audioPrefs.putBool("thrAuto", autoThresholdEnabled);
    audioPrefs.putUChar("cpuFreq", cpuFrequencyMhz);
    audioPrefs.putFloat("wifiTxDbm", wifiTxPowerDbm);
    audioPrefs.putBool("hpEnable", highpassEnabled);
    audioPrefs.putUInt("hpCutoff", (uint32_t)highpassCutoffHz);
    audioPrefs.putBool("ohEnable", overheatProtectionEnabled);
    uint32_t ohLimit = (uint32_t)(overheatShutdownC + 0.5f);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    audioPrefs.putUInt("ohThresh", ohLimit);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Compute recommended minimum packet-rate threshold based on current sample rate and buffer size
uint32_t computeRecommendedMinRate() {
    uint32_t buf = max((uint16_t)1, currentBufferSize);
    float expectedPktPerSec = (float)currentSampleRate / (float)buf;
    uint32_t rec = (uint32_t)(expectedPktPerSec * 0.7f + 0.5f); // 70% safety margin
    if (rec < 5) rec = 5;
    return rec;
}

// Restore application settings to safe defaults and persist
void resetToDefaultSettings() {
    simplePrintln("FACTORY RESET: Restoring default settings...");

    // Clear persisted settings in our namespace
    audioPrefs.begin("audio", false);
    audioPrefs.clear();
    audioPrefs.end();

    // Reset runtime variables to defaults
    currentSampleRate = DEFAULT_SAMPLE_RATE;
    currentGainFactor = DEFAULT_GAIN_FACTOR;
    currentBufferSize = DEFAULT_BUFFER_SIZE;
    i2sShiftBits = 12;  // compile-time default respected

    autoRecoveryEnabled = true;
    autoThresholdEnabled = true;
    scheduledResetEnabled = false;
    resetIntervalHours = 24;
    minAcceptableRate = computeRecommendedMinRate();
    performanceCheckInterval = 15;
    cpuFrequencyMhz = 160;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
    highpassEnabled = DEFAULT_HPF_ENABLED;
    highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLockoutActive = false;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;

    isStreaming = false;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// (Re)allocate the outgoing RTP packet buffer for the current buffer size.
// Only called from the loop task (same task that sends), so no locking is needed.
static bool allocPacketBuffer() {
    size_t need = 4 + 12 + (size_t)currentBufferSize * sizeof(int16_t);
    if (rtpPacketBuffer && rtpPacketBufferSize == need) return true;
    if (rtpPacketBuffer) { free(rtpPacketBuffer); rtpPacketBuffer = nullptr; rtpPacketBufferSize = 0; }
    rtpPacketBuffer = (uint8_t*)malloc(need);
    if (!rtpPacketBuffer) return false;
    rtpPacketBufferSize = need;
    return true;
}

// Restart I2S with new parameters
void restartI2S() {
    simplePrintln("Restarting I2S with new parameters...");
    // A running stream's SDP (sample rate) may no longer be valid, and the client would
    // otherwise sit on a silent connection until it times out. Close it so the client
    // reconnects right away with the new settings.
    if (isStreaming && rtspClient && rtspClient.connected()) {
        rtspClient.stop();
        simplePrintln("Closed active RTSP stream; client will reconnect with new settings");
    }
    isStreaming = false;

    // Stop the capture task from touching the driver/DSP while we rebuild it
    audioPause();
    setup_i2s_driver();
    computeHighpassCoeffsLocked();
    if (audioRing) xStreamBufferReset(audioRing);
    audioResume();

    if (!allocPacketBuffer()) {
        simplePrintln("FATAL: Memory allocation failed after parameter change!");
        ESP.restart();
    }
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
}

// Minimal print helpers: Serial + buffered for Web UI
void simplePrint(String message) {
    Serial.print(message);
}

void simplePrintln(String message) {
    Serial.println(message);
    webui_pushLog(message);
}

// OTA setup
void setupOTA() {
    ArduinoOTA.setHostname("ESP32-RTSP-Mic");
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.begin();
}

// DMA overrun callback (runs in ISR context): the capture task fell behind and the
// driver discarded the oldest DMA buffer.
static bool onI2SRecvOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    i2sOverflowCount = i2sOverflowCount + 1;
    return false;
}

// I2S setup. Caller must have paused the capture task (audioPause) once it is running.
void setup_i2s_driver() {
    i2sReady = false;
    if (i2sRxChan) {
        i2s_channel_disable(i2sRxChan);
        i2s_del_channel(i2sRxChan);
        i2sRxChan = nullptr;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;                       // 8 x 256 samples = ~43 ms of DMA headroom
    chan_cfg.dma_frame_num = CAPTURE_CHUNK_SAMPLES;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &i2sRxChan);
    if (err != ESP_OK) {
        simplePrintln("ERROR: i2s_new_channel failed (" + String((int)err) + ")");
        i2sRxChan = nullptr;
        return;
    }

    // Philips I2S, 32-bit slots, mono LEFT (ICS-43434 with L/R pin tied to GND).
    // Same on-wire format as the previous legacy-driver config (STAND_I2S, ONLY_LEFT).
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(currentSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DOUT_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // C6 default for mono is BOTH; force left

    err = i2s_channel_init_std_mode(i2sRxChan, &std_cfg);
    if (err == ESP_OK) {
        i2s_event_callbacks_t cbs = {};
        cbs.on_recv_q_ovf = onI2SRecvOverflow;
        i2s_channel_register_event_callback(i2sRxChan, &cbs, NULL);
        err = i2s_channel_enable(i2sRxChan);
    }
    if (err != ESP_OK) {
        simplePrintln("ERROR: I2S init failed (" + String((int)err) + ")");
        i2s_del_channel(i2sRxChan);
        i2sRxChan = nullptr;
        return;
    }
    i2sReady = true;

    // (5) log i2sShiftBits for easier debugging
    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits));
}

// Write the whole buffer or fail. Retries short/zero writes until SEND_TIMEOUT_MS so a
// brief Wi-Fi stall does not abort the stream mid-packet.
static bool writeAll(WiFiClient &client, const uint8_t* data, size_t len) {
    size_t off = 0;
    unsigned long start = millis();
    while (off < len) {
        int w = client.write(data + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            start = millis();  // progress made: restart the stall timer
            continue;
        }
        if (!client.connected() || (millis() - start) > SEND_TIMEOUT_MS) return false;
        delay(2);
    }
    return true;
}

// Build one contiguous frame ('$' interleave header + RTP header + payload) in
// rtpPacketBuffer and send it in a single write. The payload must already be in
// rtpPacketBuffer[16..] as host-order int16 samples.
// If the write cannot complete, the TCP framing is no longer trustworthy, so the
// connection is closed; the client (BirdNET-Go/ffmpeg) then reconnects cleanly
// instead of hanging on a half-sent packet.
static bool sendRTPPacket(WiFiClient &client, int numSamples) {
    if (!client.connected()) return false;

    const uint16_t payloadSize = (uint16_t)(numSamples * (int)sizeof(int16_t));
    const uint16_t packetSize = (uint16_t)(12 + payloadSize);
    uint8_t* b = rtpPacketBuffer;

    // RTSP interleaved header: '$' 0x24, channel 0, length
    b[0] = 0x24;
    b[1] = 0x00;
    b[2] = (uint8_t)((packetSize >> 8) & 0xFF);
    b[3] = (uint8_t)(packetSize & 0xFF);

    // RTP header (12 bytes)
    uint8_t* h = b + 4;
    h[0] = 0x80;      // V=2, P=0, X=0, CC=0
    h[1] = 96;        // M=0, PT=96 (dynamic)
    h[2] = (uint8_t)((rtpSequence >> 8) & 0xFF);
    h[3] = (uint8_t)(rtpSequence & 0xFF);
    h[4] = (uint8_t)((rtpTimestamp >> 24) & 0xFF);
    h[5] = (uint8_t)((rtpTimestamp >> 16) & 0xFF);
    h[6] = (uint8_t)((rtpTimestamp >> 8) & 0xFF);
    h[7] = (uint8_t)(rtpTimestamp & 0xFF);
    h[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
    h[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
    h[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
    h[11] = (uint8_t)(rtpSSRC & 0xFF);

    // Host->network: per-sample byte-swap (16bit PCM L16 big-endian)
    uint8_t* pl = b + 16;
    for (int i = 0; i < numSamples; ++i) {
        uint8_t lo = pl[2 * i];
        pl[2 * i] = pl[2 * i + 1];
        pl[2 * i + 1] = lo;
    }

    if (!writeAll(client, b, 4 + (size_t)packetSize)) {
        sendFailCount++;
        isStreaming = false;
        client.stop();
        simplePrintln("RTSP send stalled > " + String(SEND_TIMEOUT_MS) +
                      " ms (weak Wi-Fi?) - closed connection so the client can reconnect");
        return false;
    }

    rtpSequence++;
    rtpTimestamp += (uint32_t)numSamples;
    audioPacketsSent++;
    return true;
}

// Convert one raw I2S chunk to 16-bit PCM (or test tone) and update metering.
// Runs in the capture task.
static void processCaptureChunk(int samples) {
    if (highpassEnabled != hpfConfigEnabled ||
        (highpassEnabled && (hpfConfigSampleRate != currentSampleRate || hpfConfigCutoff != highpassCutoffHz))) {
        computeHighpassCoeffsLocked();
    }

    bool clipped = false;
    float peakAbs = 0.0f;

    if (testToneEnabled) {
        // Clean sine at -12 dBFS, bypassing shift/HPF/gain. The I2S clock still paces
        // it, so this exercises the exact same timing + network path as the mic.
        uint32_t inc = (uint32_t)(((uint64_t)testToneHz << 32) / currentSampleRate);
        for (int i = 0; i < samples; i++) {
            int16_t v = sineTable[tonePhase >> 24];
            tonePhase += inc;
            captureOut[i] = v;
            float a = fabsf((float)v);
            if (a > peakAbs) peakAbs = a;
        }
    } else {
        const float gain = currentGainFactor;
        const uint8_t shift = i2sShiftBits;
        const bool hp = highpassEnabled;
        for (int i = 0; i < samples; i++) {
            float sample = (float)(captureRaw[i] >> shift);
            if (hp) sample = hpf.process(sample);
            float amplified = sample * gain;
            float aabs = fabsf(amplified);
            if (aabs > peakAbs) peakAbs = aabs;
            if (amplified > 32767.0f) { amplified = 32767.0f; clipped = true; }
            if (amplified < -32768.0f) { amplified = -32768.0f; clipped = true; }
            captureOut[i] = (int16_t)amplified;
        }
    }

    // Update metering after processing the block
    if (peakAbs > 32767.0f) peakAbs = 32767.0f;
    lastPeakAbs16 = (uint16_t)peakAbs;
    audioClippedLastBlock = clipped;
    if (clipped) audioClipCount++;

    // Peak hold for a short window (~3 s) to match UI polling cadence
    if (lastPeakAbs16 > peakHoldAbs16) {
        peakHoldAbs16 = lastPeakAbs16;
        peakHoldUntilMs = millis() + 3000UL;
    } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
        peakHoldAbs16 = 0;
    }
}

// Capture task: always reading the mic so the DMA never overruns, regardless of
// what the network or Web UI is doing.
static void captureTask(void*) {
    for (;;) {
        if (capturePauseReq) {
            capturePaused = true;
            while (capturePauseReq) vTaskDelay(1);
            capturePaused = false;
            continue;
        }
        if (!i2sReady) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytesRead = 0;
        esp_err_t r = i2s_channel_read(i2sRxChan, captureRaw, sizeof(captureRaw), &bytesRead, 100);
        int samples = (r == ESP_OK) ? (int)(bytesRead / sizeof(int32_t)) : 0;
        if (samples > 0) {
            processCaptureChunk(samples);

            if (isStreaming && audioRing) {
                size_t want = (size_t)samples * sizeof(int16_t);
                size_t space = xStreamBufferSpacesAvailable(audioRing) & ~(size_t)1;
                size_t put = (want <= space) ? want : space;
                if (put) xStreamBufferSend(audioRing, captureOut, put, 0);
                if (put < want) ringDroppedSamples = ringDroppedSamples + (uint32_t)((want - put) / sizeof(int16_t));
            }
        }
        if (samples == 0) vTaskDelay(1);
    }
}

// Clear the ring and diagnostics at stream start (called from loop task on PLAY).
static void resetAudioPipelineForStream() {
    audioPause();
    if (audioRing) xStreamBufferReset(audioRing);
    i2sOverflowCount = 0;
    ringDroppedSamples = 0;
    ringPeakFillPct = 0;
    audioResume();
}

// Audio streaming: send every complete packet waiting in the ring buffer.
// Bounded per call so the Web UI/OTA keep getting serviced.
void streamAudio(WiFiClient &client) {
    if (!isStreaming || !client.connected() || !audioRing || !rtpPacketBuffer) return;

    const size_t payloadBytes = (size_t)currentBufferSize * sizeof(int16_t);
    if (4 + 12 + payloadBytes > rtpPacketBufferSize) return;  // buffer size changing

    for (int n = 0; n < 8 && isStreaming; n++) {
        size_t avail = xStreamBufferBytesAvailable(audioRing);
        uint8_t fill = (uint8_t)((avail * 100) / AUDIO_RING_BYTES);
        if (fill > ringPeakFillPct) ringPeakFillPct = fill;
        if (avail < payloadBytes) break;

        size_t got = xStreamBufferReceive(audioRing, rtpPacketBuffer + 16, payloadBytes, 0);
        if (got < sizeof(int16_t)) break;
        if (!sendRTPPacket(client, (int)(got / sizeof(int16_t)))) break;
    }
}

// RTSP handling
void handleRTSPCommand(WiFiClient &client, String request) {
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }

    lastRTSPActivity = millis();

    if (request.startsWith("OPTIONS")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n");

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        // better compatibility: include actual IP
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Content-Type: application/sdp\r\n");
        client.print("Content-Base: rtsp://" + ip + ":8554/audio/\r\n");
        client.print("Content-Length: " + String(sdp.length()) + "\r\n\r\n");
        client.print(sdp);

    } else if (request.startsWith("SETUP")) {
        rtspSessionId = String(random(100000000, 999999999));
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n");
        client.print("Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n");
        client.print("Range: npt=0.000-\r\n\r\n");

        resetAudioPipelineForStream();
        isStreaming = true;
        rtpSequence = 0;
        rtpTimestamp = 0;
        audioPacketsSent = 0;
        lastStatsReset = millis();
        lastRtspPlayMs = millis();
        rtspPlayCount++;
        simplePrintln("STREAMING STARTED");

    } else if (request.startsWith("TEARDOWN")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n\r\n");
        isStreaming = false;
        simplePrintln("STREAMING STOPPED");
    } else if (request.startsWith("GET_PARAMETER")) {
        // Many RTSP clients send GET_PARAMETER as keep-alive.
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n\r\n");
    }
}

// RTSP processing
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    if (client.available()) {
        int available = client.available();

        if (rtspParseBufferPos + available >= (int)sizeof(rtspParseBuffer)) {
            available = sizeof(rtspParseBuffer) - rtspParseBufferPos - 1;
            if (available <= 0) {
                simplePrintln("RTSP buffer overflow - resetting");
                rtspParseBufferPos = 0;
                return;
            }
        }

        client.read(rtspParseBuffer + rtspParseBufferPos, available);
        rtspParseBufferPos += available;

        char* endOfHeader = strstr((char*)rtspParseBuffer, "\r\n\r\n");
        if (endOfHeader != nullptr) {
            *endOfHeader = '\0';
            String request = String((char*)rtspParseBuffer);

            handleRTSPCommand(client, request);

            int headerLen = (endOfHeader - (char*)rtspParseBuffer) + 4;
            memmove(rtspParseBuffer, rtspParseBuffer + headerLen, rtspParseBufferPos - headerLen);
            rtspParseBufferPos -= headerLen;
        }
    }
}


// Web UI is a separate module (WebUI.*)

void setup() {
    Serial.begin(115200);
    delay(100);

    // (4) seed for random(): combination of time and unique MAC
    randomSeed((uint32_t)micros() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF));

    bootTime = millis(); // Store boot time
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);

    // Enable external antenna (for XIAO ESP32-C6).
    // NOTE: If you are using a board without the RF switch (or no external antenna), comment out or remove this block.
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    Serial.println("RF switch control enabled (GPIO3 LOW)");
    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH);
    Serial.println("External antenna selected (GPIO14 HIGH)");

    // Load settings from flash
    loadAudioSettings();

    // Allocate the outgoing packet buffer and the capture ring
    audioRing = xStreamBufferCreate(AUDIO_RING_BYTES, 1);
    if (!allocPacketBuffer() || !audioRing) {
        simplePrintln("FATAL: Memory allocation failed!");
        ESP.restart();
    }
    for (int i = 0; i < 256; i++) {
        sineTable[i] = (int16_t)lrintf(8192.0f * sinf(2.0f * 3.14159265f * (float)i / 256.0f)); // -12 dBFS
    }

    // WiFi optimization for stable streaming
    WiFi.setSleep(false);

    WiFiManager wm;
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect("ESP32-RTSP-Mic-AP")) {
        simplePrintln("WiFi failed, restarting...");
        ESP.restart();
    }

    simplePrintln("WiFi connected: " + WiFi.localIP().toString());

    // Apply configured WiFi TX power after connect (logs once on change)
    applyWifiTxPower(true);

    setupOTA();
    setup_i2s_driver();          // capture task not running yet: no pause needed
    computeHighpassCoeffsLocked();
    if (xTaskCreate(captureTask, "audio_capture", CAPTURE_TASK_STACK, nullptr,
                    CAPTURE_TASK_PRIORITY, &captureTaskHandle) != pdPASS) {
        simplePrintln("FATAL: could not start audio capture task");
        ESP.restart();
    }

    if (!overheatLatched) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    } else {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    // Web UI
    webui_begin();

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    float initialTemp = temperatureRead();
    if (isTemperatureValid(initialTemp)) {
        maxTemperature = initialTemp;
        lastTemperatureC = initialTemp;
        lastTemperatureValid = true;
        overheatSensorFault = false;
    } else {
        maxTemperature = 0.0f;
        lastTemperatureC = 0.0f;
        lastTemperatureValid = false;
        overheatSensorFault = true;
        overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
        overheatLastTimestamp = "";
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        persistOverheatNote();
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    setCpuFrequencyMhz(cpuFrequencyMhz);
    simplePrintln("CPU frequency set to " + String(cpuFrequencyMhz) + " MHz for optimal thermal/performance balance");

    if (!overheatLatched) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL: rtsp://" + WiFi.localIP().toString() + ":8554/audio");
    } else {
        simplePrintln("RTSP server paused due to thermal latch. Clear via Web UI before resuming streaming.");
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
}

void loop() {
    ArduinoOTA.handle();

    webui_handleClient();

    if (millis() - lastTempCheck > 60000) { // 1 min
        checkTemperature();
        lastTempCheck = millis();
    }

    if (millis() - lastMemoryCheck > 30000) { // 30 s
        uint32_t currentHeap = ESP.getFreeHeap();
        if (currentHeap < minFreeHeap) minFreeHeap = currentHeap;
        lastMemoryCheck = millis();
    }

    if (millis() - lastPerformanceCheck > (performanceCheckInterval * 60000UL)) {
        checkPerformance();
        lastPerformanceCheck = millis();
    }

    if (millis() - lastWiFiCheck > 30000) { // 30 s
        checkWiFiHealth(); // without TX power log spam
        lastWiFiCheck = millis();
    }

    checkScheduledReset();

    // RTSP client management
    if (rtspServerEnabled) {
        if (rtspClient && !rtspClient.connected()) {
            rtspClient.stop();
            isStreaming = false;
            simplePrintln("RTSP client disconnected");
        }

        // Timeout for RTSP clients (30 seconds of inactivity)
        if (rtspClient && rtspClient.connected() && !isStreaming) {
            if (millis() - lastRTSPActivity > 30000) {
                rtspClient.stop();
                simplePrintln("RTSP client timeout - disconnected");
            }
        }

        if (!rtspClient || !rtspClient.connected()) {
            WiFiClient newClient = rtspServer.accept();
            if (newClient) {
                rtspClient = newClient;
                rtspClient.setNoDelay(true);
                rtspParseBufferPos = 0;
                lastRTSPActivity = millis();
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                simplePrintln("New RTSP client connected from: " + rtspClient.remoteIP().toString());
            }
        }

        if (rtspClient && rtspClient.connected()) {
            if (rtspClient.available()) {
                lastRTSPActivity = millis();
            }
            processRTSP(rtspClient);
            if (isStreaming) {
                streamAudio(rtspClient);
            }
        }
    } else {
        if (rtspClient && rtspClient.connected()) {
            rtspClient.stop();
            isStreaming = false;
        }
    }
    // Handle deferred reboot/reset safely here
    if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt) {
        if (scheduledFactoryReset) {
            resetToDefaultSettings();
        }
        delay(50);
        ESP.restart();
    }

    // Yield briefly: audio is buffered by the capture task, so this costs nothing
    // and keeps the idle task (and its watchdog) happy.
    delay(1);
}
