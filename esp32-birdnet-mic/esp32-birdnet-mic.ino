#include "WifiAp.h"
#include "Antenna.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "DeviceSetup.h"
#include "Battery.h"
#include "FieldPower.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <time.h>
#include <esp_sntp.h>
#include <stdlib.h>
#include <math.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <WiFiUdp.h>
#include <errno.h>
#include <atomic>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "WebUI.h"
#include "MicrophoneProfiles.h"
#include "S3Boards.h"
#include "TimeZoneSchedule.h"
#include "DiscoveryProgress.h"
#include "MqttDiscovery.h"
#include "DiscoveryClient.h"
#include "CooperativeTcpWriter.h"
#include "StreamHealth.h"
#include "AudioDiagnostics.h"
#include "MicrophoneSignal.h"
#include "BrowserAudio.h"
#include "Recording.h"
#include "RecordingPins.h"
static uint32_t recordingDmaBaseline = 0;
static bool recordingAutoStartPending = false;
#include "ThermalRecovery.h"
#include "ChipTemperature.h"
#include "FlashLayout.h"
#include "AudioReserve.h"
#include <esp_memory_utils.h>

// XIAO ESP32-C5 with PSRAM enabled: the RTSP audio ring (and the browser-listening
// queue, BrowserAudio.cpp) go in its 8 MB PSRAM, so internal RAM stays free for
// Wi-Fi bursts. DMA and PDM working buffers stay internal. Needs board option
// PSRAM=enabled (-DBOARD_HAS_PSRAM; tools/compile_firmware.sh adds it for C5).
// Without it the core clears CONFIG_SPIRAM and the internal-RAM path is used.
#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_SPIRAM
#define AUDIO_RING_IN_PSRAM 1
#else
#define AUDIO_RING_IN_PSRAM 0
#endif
#ifndef LOG_MEMORY_TREND
#define LOG_MEMORY_TREND 0  // 1: log heap, PSRAM and stream counters every 5 minutes
#endif
#define FXAUDIO_HOT IRAM_ATTR __attribute__((noinline))
#include "FixedAudio.h"

#if SOC_I2S_SUPPORTS_PDM2PCM
#define MIC_PDM_HARDWARE 1
#else
#define MIC_PDM_HARDWARE 0
#endif

// PDM capability follows the installed ESP-IDF target, not a guessed board name.
#if SOC_I2S_SUPPORTS_PDM_RX && !SOC_I2S_SUPPORTS_PDM2PCM
#define MIC_PDM_SOFTWARE 1
#define PDMSW_HOT IRAM_ATTR __attribute__((noinline))
#include "PdmCapture.h"
struct PdmReadBuffers {
    uint8_t bytes[2048];
    uint16_t words[1025]; // +1 for a carried byte from a short DMA read
};
static pdmsw::Decimator* pdmDecimator = nullptr;
static PdmReadBuffers* pdmReadBuffers = nullptr;
static pdmsw::WordFramer pdmFramer;
static pdmsw::WordRateDetector pdmWordRate;
static uint32_t pdmReadChunks = 0;
static std::atomic<uint8_t> pdmDetectedStride{0};
static std::atomic<uint8_t> pdmStrideLogPending{0};
static std::atomic<uint8_t> pdmDetectedOrder{0};
#else
#define MIC_PDM_SOFTWARE 0
#endif
static constexpr uint32_t PDM_SAMPLE_RATE = 48000;
static constexpr uint32_t PDM_CLOCK_HZ = PDM_SAMPLE_RATE * 64;

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi) ==================
#define FW_VERSION "2.00"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;
// Build timestamp for diagnostics (compile time)
const char* FW_BUILD_DATE_STR = __DATE__ " " __TIME__;

#if defined(ARDUINO_XIAO_ESP32C3)
#define XIAO_BOARD_ID "xiao-esp32c3"
#define XIAO_BOARD_NAME "XIAO ESP32-C3"
#define XIAO_CHIP_FAMILY "ESP32-C3"
#define XIAO_OTA_ARTIFACT "firmware-app-c3.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32S3)
#define XIAO_BOARD_ID "xiao-esp32s3"
#define XIAO_BOARD_NAME "XIAO ESP32-S3"
#define XIAO_CHIP_FAMILY "ESP32-S3"
#define XIAO_OTA_ARTIFACT "firmware-app-s3.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32C5)
#define XIAO_BOARD_ID "xiao-esp32c5"
#define XIAO_BOARD_NAME "XIAO ESP32-C5"
#define XIAO_CHIP_FAMILY "ESP32-C5"
#define XIAO_OTA_ARTIFACT "firmware-app-c5.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32C6)
#define XIAO_BOARD_ID "xiao-esp32c6"
#define XIAO_BOARD_NAME "XIAO ESP32-C6"
#define XIAO_CHIP_FAMILY "ESP32-C6"
#define XIAO_OTA_ARTIFACT "firmware-app-c6.bin"
#define XIAO_HAS_RF_SWITCH 1
#else
#define XIAO_BOARD_ID "esp32-generic"
#define XIAO_BOARD_NAME "ESP32 generic"
#define XIAO_CHIP_FAMILY "ESP32"
#define XIAO_OTA_ARTIFACT ""
#define XIAO_HAS_RF_SWITCH 0
#endif

const char* FW_BOARD_ID_STR = XIAO_BOARD_ID;
const char* FW_BOARD_NAME_STR = XIAO_BOARD_NAME;
const char* FW_CHIP_FAMILY_STR = XIAO_CHIP_FAMILY;
const char* FW_OTA_ARTIFACT_STR = XIAO_OTA_ARTIFACT;

// Time / NTP
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
static const unsigned long NTP_SYNC_INTERVAL_SYNCED_MS = 6UL * 60UL * 60UL * 1000UL;   // 6 hours
static const unsigned long NTP_SYNC_INTERVAL_UNSYNCED_MS = 30000UL;
static const unsigned long NTP_MIN_REQUEST_INTERVAL_MS = 15000UL;
bool timeSyncEnabled = true;
bool timeSynced = false;                 // true after the first successful NTP sync
unsigned long lastTimeSyncAttempt = 0;   // millis() of last attempt
unsigned long lastTimeSyncSuccess = 0;   // millis() of last success
// The lwIP callback only queues an event. All application state/logging is loop-owned.
static std::atomic<uint32_t> ntpSyncEvents{0};
static bool ntpConfigured = false, ntpServiceEnabled = false;
static bool ntpAttempted = false, ntpWasConnected = false, ntpReconnectPending = false;
int32_t timeOffsetMinutes = 0;           // user-set offset applied to displayed time
uint8_t timeZoneMode = TIME_ZONE_FIXED;
String customTimeZoneRule = "UTC0";

// mDNS
String mdnsHostname = "esp32mic"; // results in <hostname>.local
bool mdnsEnabled = true;
bool mdnsRunning = false;

// OTA password (optional):
// - For production, set a strong password to protect OTA updates.
// - You can leave it undefined to disable password protection.
// - Example placeholder (edit as needed):
// #define OTA_PASSWORD "1234"  // Optional: change or leave undefined

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
static const uint8_t OVERHEAT_CONFIRM_SAMPLES = 3;
static const float OVERHEAT_EMERGENCY_MARGIN_C = 10.0f;
static const unsigned long TEMPERATURE_CHECK_INTERVAL_MS = 5000UL;

// -- Pins
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3) || \
    defined(ARDUINO_XIAO_ESP32C5) || defined(ARDUINO_XIAO_ESP32C6)
static constexpr int I2S_MCLK_PIN = D7;
static constexpr int I2S_BCLK_PIN = D3;
static constexpr int I2S_LRCLK_PIN = D1;
static constexpr int I2S_DOUT_PIN = D2;
#else
static constexpr int I2S_MCLK_PIN = 7;
static constexpr int I2S_BCLK_PIN = 21;
static constexpr int I2S_LRCLK_PIN = 1;
static constexpr int I2S_DOUT_PIN = 2;
#endif

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

static const uint8_t MAX_CLIENTS = 3;

enum TransportMode : uint8_t {
    TRANSPORT_TCP = 0,
    TRANSPORT_UDP = 1
};

enum StreamTarget : uint8_t {
    STREAM_TARGET_BIRDNET_GO = 0,
    STREAM_TARGET_BIRDNET_PI = 1
};

enum MicFormat : uint8_t {
    MIC_FORMAT_PHILIPS = 0,  // ICS-43434 / INMP441
    MIC_FORMAT_MSB = 1,      // Adafruit SPH0645LM4H
    MIC_FORMAT_PDM = 2       // IM73D122, 48 kHz PCM
};

static constexpr uint8_t DEFAULT_MIC_FORMAT = MIC_FORMAT_PHILIPS;

struct StreamProfileConfig {
    uint8_t target = STREAM_TARGET_BIRDNET_GO;
};

struct ClientSession {
    WiFiClient client;
    WiFiUDP udpSocket;
    WiFiUDP rtcpSocket;
    TransportMode transport = TRANSPORT_TCP;
    IPAddress clientRtpAddress = IPAddress();
    uint16_t clientRtpPort = 0;
    uint16_t serverRtpPort = 0;
    String sessionId = "";
    bool streaming = false;
    uint8_t profileIndex = 0;  // 0 => /audio1 (alias /audio), 1 => /audio2
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;
    unsigned long lastActivity = 0;
    unsigned long connectedAtMs = 0;
    unsigned long playStartedAtMs = 0;
    unsigned long lastRtpPacketMs = 0;
    char userAgent[64] = {0};
    uint8_t parseBuffer[1024] = {0};
    int parseBufferPos = 0;
    uint16_t interleavedBytesRemaining = 0;
    unsigned long packetsSent = 0;
    CooperativeTcpWriter tcpWriter;
    bool tcpFrameIsRtp = false;
    uint16_t tcpFrameSamples = 0;
    uint32_t tcpFrameChunk = 0;
    uint32_t tcpFrameDropsAtQueue = 0;

    void reset() {
        tcpWriter.release();
        tcpFrameIsRtp = false;
        tcpFrameSamples = 0;
        tcpFrameChunk = 0;
        tcpFrameDropsAtQueue = 0;
        client.stop();
        if (transport == TRANSPORT_UDP) {
            udpSocket.stop();
            rtcpSocket.stop();
        }
        transport = TRANSPORT_TCP;
        clientRtpPort = 0;
        serverRtpPort = 0;
        sessionId = "";
        streaming = false;
        profileIndex = 0;
        rtpSequence = 0;
        rtpTimestamp = 0;
        lastActivity = 0;
        connectedAtMs = 0;
        playStartedAtMs = 0;
        lastRtpPacketMs = 0;
        userAgent[0] = '\0';
        parseBufferPos = 0;
        interleavedBytesRemaining = 0;
        parseBuffer[0] = '\0';
        packetsSent = 0;
    }
};

// Runtime microphone wiring selection within the shared S3 image.
uint8_t micFormat = DEFAULT_MIC_FORMAT;
uint8_t s3BoardVariant = s3boards::External;
bool s3BoardSelectionSupported() {
#if defined(ARDUINO_XIAO_ESP32S3)
    return true;
#else
    return false;
#endif
}
bool builtinMicSelected() {
    return s3BoardSelectionSupported() && s3boards::builtin(s3BoardVariant) && micFormat == 2;
}
bool micBoardFormatAllowed(uint8_t format) {
    return !s3BoardSelectionSupported() || s3BoardVariant != s3boards::AtomS3U || format == 2;
}
void updateS3BoardIdentity() {
    if (!s3BoardSelectionSupported()) return;
    FW_BOARD_ID_STR = s3boards::id(s3BoardVariant);
    FW_BOARD_NAME_STR = s3boards::name(s3BoardVariant);
}

bool getRecommendedMicSettings(uint8_t format, micprofiles::Settings& out) {
#if defined(ARDUINO_XIAO_ESP32C3)
    constexpr auto board = micprofiles::C3;
#elif defined(ARDUINO_XIAO_ESP32S3)
    constexpr auto board = micprofiles::S3;
#elif defined(ARDUINO_XIAO_ESP32C5)
    constexpr auto board = micprofiles::C5;
#elif defined(ARDUINO_XIAO_ESP32C6)
    constexpr auto board = micprofiles::C6;
#else
    constexpr auto board = micprofiles::Unknown;
#endif
    if (!micBoardFormatAllowed(format) || !micprofiles::recommended(board, format, out)) return false;
    // Start conservatively with the built-in microphones; tune from the level meter.
    if (s3BoardSelectionSupported() && s3boards::builtin(s3BoardVariant) && format == 2) out.gain = DEFAULT_GAIN_FACTOR;
    return true;
}

uint16_t maxSupportedCpuFrequencyMhz() {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C5)
    return 240;
#else
    return 160;
#endif
}

bool cpuFrequencySupported(uint32_t mhz) {
    return mhz == 40 || mhz == 80 || mhz == 160 ||
        (mhz == 240 && maxSupportedCpuFrequencyMhz() >= 240);
}

static const char* micFormatName(uint8_t format) {
    if (format == MIC_FORMAT_PDM) return "PDM";
    return format == MIC_FORMAT_MSB ? "SPH0645/MSB" : "ICS43434/Philips";
}

bool pdmMicSupported() {
#if SOC_I2S_SUPPORTS_PDM_RX
    return true;
#else
    return false;
#endif
}

const char* pdmDecoderName() {
#if SOC_I2S_SUPPORTS_PDM2PCM
    return "hardware";
#elif MIC_PDM_SOFTWARE
    return "software";
#else
    return "unavailable";
#endif
}

uint8_t pdmRawWordStride() {
#if MIC_PDM_SOFTWARE
    return pdmDetectedStride.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

const char* pdmRawBitOrder() {
#if MIC_PDM_SOFTWARE
    uint8_t order = pdmDetectedOrder.load(std::memory_order_relaxed);
    return order == pdmsw::ORDER_MSB_FIRST ? "msb" : order == pdmsw::ORDER_LSB_FIRST ? "lsb" : "detecting";
#else
    return "hardware";
#endif
}

static void releasePdmCapture() {
#if MIC_PDM_SOFTWARE
    free(pdmDecimator); pdmDecimator = nullptr;
    free(pdmReadBuffers); pdmReadBuffers = nullptr;
    pdmFramer.reset();
    pdmWordRate.reset();
    pdmReadChunks = 0;
    pdmDetectedStride.store(0, std::memory_order_relaxed);
    pdmStrideLogPending.store(0, std::memory_order_relaxed);
    pdmDetectedOrder.store(0, std::memory_order_relaxed);
#endif
}

ClientSession clients[MAX_CLIENTS];
StreamProfileConfig streamProfiles[2] = {
    {STREAM_TARGET_BIRDNET_GO},
    {STREAM_TARGET_BIRDNET_PI},
};
bool streamEnabled[2] = {true, false};
uint8_t maxActiveClients = 2;

struct StreamStats {
    uint8_t clientCount = 0;
    bool streaming = false;
    uint32_t packetsSent = 0;
    unsigned long statsResetMs = 0;
    unsigned long lastConnectMs = 0;
    unsigned long lastPlayMs = 0;
};
StreamStats streamStats[2];
static uint32_t audioChunkSequence = 0;
static uint32_t countedAudioChunk = 0;
static uint32_t countedProfileChunk[2] = {0, 0};
RecentPacketRate recentAudioRate;
RecentPacketRate recentStreamRate[2];

void stopAllRtspClients(const char* reason);
void stopRtspClientsForStream(uint8_t profileIndex, const char* reason);
void getStreamClientCounts(uint8_t &s1, uint8_t &s2);
uint8_t getRtspClientCount();
String getRtspClientSummary();

// -- RTSP Streaming
String rtspSessionId = "";
volatile bool isStreaming = false;
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
int32_t* i2s_32bit_buffer = nullptr;
int16_t* i2s_16bit_buffer = nullptr;
int16_t* i2s_16bit_network_buffer = nullptr;
uint8_t* rtpPacketScratch = nullptr;
i2s_chan_handle_t i2s_rx_handle = nullptr;
RingbufHandle_t audioRingBuffer = nullptr;
bool audioRingInPsram = false;
TaskHandle_t audioProducerTaskHandle = nullptr;
volatile bool audioProducerStopRequested = false;
volatile bool audioProducerRunning = false;
volatile uint32_t audioProducerLastReadMs = 0;
size_t audioRingBufferCapacityBytes = 0;

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
uint8_t i2sShiftBits = 12;  // (1) compile-time default respected on first boot

MicrophoneSignal microphoneSignal;

// -- Audio metering / clipping diagnostics
uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
uint32_t audioClipCount = 0;      // total blocks where clipping occurred
bool audioClippedLastBlock = false; // clipping occurred in last processed block
uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldUntilMs = 0; // when to clear hold

// The audio producer owns all DSP state. HTTP handlers only publish settings.
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
fxaudio::BiquadQ28 hpf{};
bool hpfConfigEnabled = false;
uint32_t hpfConfigSampleRate = 0;
uint16_t hpfConfigCutoff = 0;

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics, auto-recovery and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastPerformanceCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastTempCheck = 0;
static const unsigned long WEBUI_HANDLE_INTERVAL_MS = 20;
unsigned long lastWebuiHandleMs = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = true;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;

// Deferred WiFi reconnect
volatile unsigned long wifiReconnectAt = 0;
bool wifiReconnectHasBssid = false;
uint8_t wifiReconnectBssid[6] = {0};
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
unsigned long lastProducerHealthCheck = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
uint8_t overheatConsecutiveSamples = 0;
float overheatTripTemp = 0.0f;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason = "";
String overheatLastTimestamp = "";
bool overheatSensorFault = false;
bool overheatLatched = false;
bool overheatAutoResumeEnabled = false;
bool otaInProgress = false;
ThermalRecovery thermalRecovery;

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Stream schedule (local clock, minutes from midnight)
bool streamScheduleEnabled = false;
uint16_t streamScheduleStartMin = 0; // 00:00
uint16_t streamScheduleStopMin = 0;  // 00:00 (same as start = empty/blocked window)
unsigned long lastStreamScheduleCheck = 0;
bool lastScheduleAllow = true;
bool lastScheduleTimeValid = false;
unsigned long lastScheduleUnsyncedLog = 0;

// -- Optional deep sleep outside stream schedule window (conservative mode)
bool deepSleepScheduleEnabled = false;
unsigned long deepSleepOutsideSinceMs = 0;
String deepSleepStatusCode = "disabled";
uint32_t deepSleepNextSleepSec = 0;
static const unsigned long DEEP_SLEEP_BOOT_GRACE_MS = 120000UL;      // 2 min after boot
static const unsigned long DEEP_SLEEP_OUTSIDE_STABLE_MS = 30000UL;    // 30 s outside window before sleep
static const uint32_t DEEP_SLEEP_MIN_SEC = 120UL;                     // minimum timer sleep
static const uint32_t DEEP_SLEEP_MAX_SEC = 28800UL;                   // cap one sleep chunk to 8 h
static const uint32_t DEEP_SLEEP_DRIFT_GUARD_SEC = 300UL;             // wake 5 min before window start

// Deep-sleep snapshot retained across deep-sleep reset (for post-wake logging).
static const uint32_t DEEP_SLEEP_SNAPSHOT_MAGIC = 0x44535031UL; // "DSP1"
RTC_DATA_ATTR uint32_t rtcSleepSnapshotMagic = 0;
RTC_DATA_ATTR uint32_t rtcSleepPlannedSec = 0;
RTC_DATA_ATTR uint32_t rtcSleepUntilStartSec = 0;
RTC_DATA_ATTR uint16_t rtcSleepStartMin = 0;
RTC_DATA_ATTR uint16_t rtcSleepStopMin = 0;
RTC_DATA_ATTR uint16_t rtcSleepEnteredMin = 0;
RTC_DATA_ATTR int32_t rtcSleepOffsetMin = 0;
RTC_DATA_ATTR uint32_t rtcSleepCycleCount = 0;

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum recent output rate for a warning
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = 160;          // CPU frequency (default 160 MHz)

// Forward declaration (used by early wake-snapshot logger).
void simplePrintln(String message);
void scheduleReboot(bool factoryReset, uint32_t delayMs);
void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs);
void mqttRequestReconnect(bool forceDiscovery);
void mqttPublishDiscoverySoon();
void saveAudioSettings();
void cancelThermalResume();

// -- WiFi TX power (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;
uint32_t wifiReconnectCount = 0;
uint32_t restartCounter = 0;
String rebootReason = "unknown";
uint32_t audioI2SErrorCount = 0;
std::atomic<uint32_t> audioProcessingLoadSnapshot{0};
static DRAM_ATTR portMUX_TYPE audioDmaMux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t audioDmaOverrunTotal = 0; // ISR-only writer, protected by audioDmaMux
static DRAM_ATTR bool audioDmaCountingEnabled = false;
static bool audioDmaCallbackAvailable = false;
static RecentDmaOverruns audioDmaWindow;

// Shared between capture, RTSP recovery and Web UI; no other state is published
// through this counter, so relaxed ordering is sufficient.
std::atomic<uint32_t> audioRingBufferDropCount{0};
uint32_t audioRingBufferChunkCount = 0;
uint32_t audioRingBufferFlushCount = 0;
bool audioRingBufferFlushPending = false;
uint32_t rtspWriteStallCount = 0;
uint32_t rtspWriteTimeoutCount = 0;

// -- MQTT (Home Assistant discovery + telemetry)
bool mqttEnabled = false;
String mqttHost = "";
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUser = "";
String mqttPassword = "";
String mqttTopicPrefix = "";
String mqttDiscoveryPrefix = "homeassistant";
String mqttClientId = "";
uint16_t mqttPublishIntervalSec = DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
bool mqttConnected = false;
String mqttLastError = "disabled";
String mqttDeviceId = "";
DiscoveryClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);
IPAddress mqttResolvedIp;
String mqttResolvedHost = "";
bool mqttResolvedIpValid = false;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long lastMqttLogMs = 0;
bool mqttDiscoveryPublished = false;
bool mqttForceDiscovery = false;
DiscoveryProgress mqttDiscoveryProgress;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;
static const unsigned long MQTT_STREAMING_RECONNECT_INTERVAL_MS = 300000UL;
static const int32_t MQTT_STREAMING_CONNECT_TIMEOUT_MS = 500;
static const uint16_t MQTT_SOCKET_TIMEOUT_SEC = 1;
static const uint16_t MQTT_PUBLISH_INTERVAL_MIN_SEC = 10;
static const uint16_t MQTT_PUBLISH_INTERVAL_MAX_SEC = 3600;

// WiFi reconnect timing
static const unsigned long WIFI_RECONNECT_SETTLE_MS = 100UL;
static const unsigned long WIFI_RECONNECT_TIMEOUT_MS = 8000UL;
static const unsigned long WIFI_RECONNECT_POLL_MS = 100UL;

// -- RTSP diagnostics (for clearer disconnect reasons in logs)
unsigned long lastRtspCommandMs = 0;
String lastRtspCommand = "none";
unsigned long streamStartedAtMs = 0;
unsigned long lastRtpPacketMs = 0;
String lastStreamStopReason = "none";
unsigned long lastStreamStopMs = 0;
uint32_t rtspWriteFailCount = 0;
uint32_t rtspTeardownCount = 0;
uint32_t rtspDisconnectCount = 0;
String lastStreamStopTime = "";
uint8_t lastStreamStopStream = 0;

void recordStreamStop(const char* reason, uint8_t stream) {
    lastStreamStopReason = reason;
    lastStreamStopMs = millis();
    lastStreamStopTime = timeSynced ? formatDateTime() : String("Clock unsynced");
    lastStreamStopStream = stream;
}
String lastRtspClientIp = "none";

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

static String resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "unknown";
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
#ifdef ESP_RST_USB
        case ESP_RST_USB:       return "usb";
#endif
#ifdef ESP_RST_JTAG
        case ESP_RST_JTAG:      return "jtag";
#endif
#ifdef ESP_RST_EFUSE
        case ESP_RST_EFUSE:     return "efuse";
#endif
#ifdef ESP_RST_PWR_GLITCH
        case ESP_RST_PWR_GLITCH:return "power_glitch";
#endif
#ifdef ESP_RST_CPU_LOCKUP
        case ESP_RST_CPU_LOCKUP:return "cpu_lockup";
#endif
        default:                return "other";
    }
}

static void loadBootMetadata() {
    rebootReason = resetReasonToString(esp_reset_reason());
    audioPrefs.begin("audio", false);
    restartCounter = audioPrefs.getUInt("bootCount", 0);
    if (restartCounter < 0xFFFFFFFFUL) restartCounter++;
    audioPrefs.putUInt("bootCount", restartCounter);
    audioPrefs.end();
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

static String bssidBytesToStr(const uint8_t b[6]) {
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return String(s);
}

static void clearStoredBssidPin() {
    if (wifiApPinned()) return;
    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return;
    if (!cur.sta.bssid_set) return;
    cur.sta.bssid_set = false;
    memset(cur.sta.bssid, 0, 6);
    if (esp_wifi_set_config(WIFI_IF_STA, &cur) == ESP_OK) {
        simplePrintln("Cleared stored WiFi BSSID pin");
    }
}

static void logConnectedAp(const char *tag) {
    if (WiFi.status() != WL_CONNECTED) return;
    simplePrintln(String(tag) + " AP: ssid=" + WiFi.SSID() +
                  " bssid=" + WiFi.BSSIDstr() +
                  " ch=" + String(WiFi.channel()) +
                  " rssi=" + String(WiFi.RSSI()) + "dBm");
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

static String mqttJsonEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') { o += "\\n"; }
        else { o += c; }
    }
    return o;
}

static bool isZeroMacBytes(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < 6; ++i) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static String macBytesToHex(const uint8_t mac[6]) {
    char buf[13];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static String buildMqttMacSuffix() {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK && !isZeroMacBytes(mac)) {
        return macBytesToHex(mac);
    }

    // Fallback for unusual cores: WiFi.macAddress() is valid after Wi-Fi init.
    String macStr = WiFi.macAddress();
    macStr.replace(":", "");
    macStr.toLowerCase();
    if (macStr != "000000000000") return macStr;

    // Last-resort fallback, formatted in standard MAC byte order.
    uint64_t efuse = ESP.getEfuseMac();
    mac[0] = (uint8_t)(efuse >> 40);
    mac[1] = (uint8_t)(efuse >> 32);
    mac[2] = (uint8_t)(efuse >> 24);
    mac[3] = (uint8_t)(efuse >> 16);
    mac[4] = (uint8_t)(efuse >> 8);
    mac[5] = (uint8_t)efuse;
    if (!isZeroMacBytes(mac)) return macBytesToHex(mac);

    return "000000000000";
}

static String defaultMdnsHostname() {
    String suffix = buildMqttMacSuffix();
    if (suffix.length() > 6) suffix = suffix.substring(suffix.length() - 6);
    suffix.toLowerCase();
    return String("esp32mic-") + suffix;
}

String sanitizeMdnsHostname(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    bool prevDash = false;
    for (size_t i = 0; i < input.length() && out.length() < 32; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) c = '-';
        if (c == '-') {
            if (out.length() == 0 || prevDash) continue;
            prevDash = true;
        } else {
            prevDash = false;
        }
        out += c;
    }
    while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
    if (out.length() == 0) return fallback;
    return out;
}

static bool isMqttTokenChar(char c) {
    return ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.');
}

static String sanitizeMqttTopicPath(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    bool prevSlash = false;
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '\t') c = '_';
        if (c == '/') {
            if (out.length() == 0 || prevSlash) continue;
            out += '/';
            prevSlash = true;
            continue;
        }
        prevSlash = false;
        if (!isMqttTokenChar(c)) c = '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out += c;
    }
    while (out.length() && out[0] == '/') out.remove(0, 1);
    while (out.length() && out[out.length() - 1] == '/') out.remove(out.length() - 1, 1);
    if (out.length() == 0) return fallback;
    return out;
}

static String sanitizeMqttClientId(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '\t') c = '_';
        if (!isMqttTokenChar(c)) c = '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out += c;
    }
    if (out.length() == 0) return fallback;
    return out;
}

static bool isHexMacSuffix(const String &s) {
    if (s.length() != 12) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

static String mqttDeviceMacSuffix() {
    const String prefix = "esp32mic_";
    if (mqttDeviceId.startsWith(prefix)) {
        String suffix = mqttDeviceId.substring(prefix.length());
        suffix.toLowerCase();
        if (isHexMacSuffix(suffix)) return suffix;
    }
    return buildMqttMacSuffix();
}

static String mqttDefaultTopicPrefix() {
    return String("esp32mic/") + mqttDeviceMacSuffix();
}

static String mqttDefaultClientId() {
    return String("esp32mic-") + mqttDeviceMacSuffix();
}

static bool mqttIsLegacyDefaultTopicPrefix(const String &value) {
    String v = value;
    v.toLowerCase();
    return v == String("esp32mic/") + mqttDeviceId ||
           v == "esp32mic/esp32mic_000000000000" ||
           v == "esp32mic_000000000000" ||
           v == "esp32mic/000000000000";
}

static bool mqttIsLegacyDefaultClientId(const String &value) {
    String v = value;
    v.toLowerCase();
    return v == String("esp32mic-") + mqttDeviceId ||
           v == "esp32mic-esp32mic_000000000000" ||
           v == "esp32mic_000000000000" ||
           v == "esp32mic-000000000000";
}

static String mqttStateTopic() {
    return mqttTopicPrefix + "/state";
}

static String mqttAvailabilityTopic() {
    return mqttTopicPrefix + "/availability";
}

static String mqttCmdRtspTopic() {
    return mqttTopicPrefix + "/cmd/rtsp_server";
}

static String mqttCmdStreamEnabledTopic(uint8_t profileIndex) {
    return mqttTopicPrefix + "/cmd/stream" + String(profileIndex + 1) + "_enabled";
}

static String mqttCmdStreamTargetTopic(uint8_t profileIndex) {
    return mqttTopicPrefix + "/cmd/stream" + String(profileIndex + 1) + "_target";
}

static String mqttCmdRebootTopic() {
    return mqttTopicPrefix + "/cmd/reboot";
}

static String streamTargetName(uint8_t target) {
    return (target == STREAM_TARGET_BIRDNET_PI) ? String("BirdNET-Pi") : String("BirdNET-Go");
}

static void mqttNormalizeSettings() {
    mqttHost.trim();
    mqttUser.trim();
    mqttClientId.trim();
    mqttTopicPrefix.trim();
    mqttDiscoveryPrefix.trim();
    if (mqttPort == 0) mqttPort = DEFAULT_MQTT_PORT;
    if (mqttDeviceId.length() == 0) {
        mqttDeviceId = sanitizeMqttClientId(String("esp32mic_") + buildMqttMacSuffix(), "esp32mic");
    }
    if (mqttIsLegacyDefaultTopicPrefix(sanitizeMqttTopicPath(mqttTopicPrefix, ""))) {
        mqttTopicPrefix = "";
    }
    if (mqttIsLegacyDefaultClientId(sanitizeMqttClientId(mqttClientId, ""))) {
        mqttClientId = "";
    }
    mqttTopicPrefix = sanitizeMqttTopicPath(mqttTopicPrefix, mqttDefaultTopicPrefix());
    mqttDiscoveryPrefix = sanitizeMqttTopicPath(mqttDiscoveryPrefix, "homeassistant");
    mqttClientId = sanitizeMqttClientId(mqttClientId, mqttDefaultClientId());
    if (mqttPublishIntervalSec < MQTT_PUBLISH_INTERVAL_MIN_SEC) mqttPublishIntervalSec = MQTT_PUBLISH_INTERVAL_MIN_SEC;
    if (mqttPublishIntervalSec > MQTT_PUBLISH_INTERVAL_MAX_SEC) mqttPublishIntervalSec = MQTT_PUBLISH_INTERVAL_MAX_SEC;
}

static String mqttBuildDeviceJson() {
    String json = "{";
    json += "\"ids\":[\"" + mqttJsonEscape(mqttDeviceId) + "\"],";
    json += "\"name\":\"ESP32 RTSP Mic\",";
    json += "\"mdl\":\"" + mqttJsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"mf\":\"Sukecz\",";
    json += "\"sw\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"cu\":\"http://" + mqttJsonEscape(WiFi.localIP().toString()) + "/\"";
    json += "}";
    return json;
}

uint32_t getCurrentPacketRate(uint8_t profileIndex) {
    uint32_t now = millis();
    if (profileIndex < 2) {
        return streamStats[profileIndex].streaming
            ? recentStreamRate[profileIndex].rate(now, streamStats[profileIndex].packetsSent) : 0;
    }
    return isStreaming ? recentAudioRate.rate(now, audioPacketsSent) : 0;
}

static String mqttBuildStateJson() {
    unsigned long nowMs = millis();
    unsigned long uptimeSeconds = (nowMs - bootTime) / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
    uint32_t currentRate = getCurrentPacketRate(255);
    uint32_t streamUptimeSeconds = (isStreaming && streamStartedAtMs > 0 && nowMs >= streamStartedAtMs)
                                       ? (uint32_t)((nowMs - streamStartedAtMs) / 1000UL)
                                       : 0;
    uint8_t clientCount = getRtspClientCount();
    uint8_t s1clients = 0, s2clients = 0;
    getStreamClientCounts(s1clients, s2clients);
    uint32_t streamRate[2] = {0, 0};
    uint32_t streamLastPlayAge[2] = {0, 0};
    for (uint8_t i = 0; i < 2; i++) {
        streamRate[i] = getCurrentPacketRate(i);
        if (streamStats[i].lastPlayMs > 0 && nowMs >= streamStats[i].lastPlayMs) {
            streamLastPlayAge[i] = (uint32_t)((nowMs - streamStats[i].lastPlayMs) / 1000UL);
        }
    }

    String json = "{";
    json += "\"fw_version\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"board_id\":\"" + mqttJsonEscape(String(FW_BOARD_ID_STR)) + "\",";
    json += "\"board_name\":\"" + mqttJsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"chip_family\":\"" + mqttJsonEscape(String(FW_CHIP_FAMILY_STR)) + "\",";
    json += "\"fw_build\":\"" + mqttJsonEscape(String(FW_BUILD_DATE_STR)) + "\",";
    json += "\"reboot_reason\":\"" + mqttJsonEscape(rebootReason) + "\",";
    json += "\"restart_counter\":" + String(restartCounter) + ",";
    json += "\"ip\":\"" + mqttJsonEscape(WiFi.localIP().toString()) + "\",";
    json += "\"wifi_ssid\":\"" + mqttJsonEscape(WiFi.SSID()) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_reconnect_count\":" + String(wifiReconnectCount) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + ",";
    json += "\"free_heap_kb\":" + String(freeHeap / 1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap / 1024) + ",";
    json += "\"uptime_s\":" + String(uptimeSeconds) + ",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled ? "true" : "false") + ",";
    json += "\"streaming\":" + String(isStreaming ? "true" : "false") + ",";
    json += "\"stream_uptime_s\":" + String(streamUptimeSeconds) + ",";
    json += "\"client_count\":" + String((uint32_t)clientCount) + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"stream1_url_ip\":\"rtsp://" + mqttJsonEscape(WiFi.localIP().toString()) + ":8554/audio1\",";
    json += "\"stream2_url_ip\":\"rtsp://" + mqttJsonEscape(WiFi.localIP().toString()) + ":8554/audio2\",";
    json += "\"stream1_url_mdns\":\"rtsp://" + mqttJsonEscape(mdnsHostname) + ".local:8554/audio1\",";
    json += "\"stream2_url_mdns\":\"rtsp://" + mqttJsonEscape(mdnsHostname) + ".local:8554/audio2\",";
    json += "\"stream1_enabled\":" + String(streamEnabled[0] ? "true" : "false") + ",";
    json += "\"stream2_enabled\":" + String(streamEnabled[1] ? "true" : "false") + ",";
    json += "\"stream1_streaming\":" + String(streamStats[0].streaming ? "true" : "false") + ",";
    json += "\"stream2_streaming\":" + String(streamStats[1].streaming ? "true" : "false") + ",";
    json += "\"stream1_clients\":" + String((uint32_t)s1clients) + ",";
    json += "\"stream2_clients\":" + String((uint32_t)s2clients) + ",";
    json += "\"stream1_packet_rate\":" + String(streamRate[0]) + ",";
    json += "\"stream2_packet_rate\":" + String(streamRate[1]) + ",";
    json += "\"stream1_target\":\"" + streamTargetName(streamProfiles[0].target) + "\",";
    json += "\"stream2_target\":\"" + streamTargetName(streamProfiles[1].target) + "\",";
    json += "\"stream1_last_play_age_s\":" + String(streamLastPlayAge[0]) + ",";
    json += "\"stream2_last_play_age_s\":" + String(streamLastPlayAge[1]) + ",";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"audio_format\":\"L16/mono\",";
    json += "\"mic_signal\":\"" + String(MicrophoneSignal::name(microphoneSignal.state(nowMs, audioProducerRunning && !audioProducerStopRequested))) + "\",";
    json += "\"mic_flatline_sec\":" + String(microphoneSignal.flatSeconds(nowMs, audioProducerRunning && !audioProducerStopRequested)) + ",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"gain\":" + String(currentGainFactor, 2) + ",";
    json += "\"client\":\"" + mqttJsonEscape(getRtspClientSummary()) + "\",";
    if (lastTemperatureValid) json += "\"temperature_c\":" + String(lastTemperatureC, 1) + ",";
    else json += "\"temperature_c\":null,";
    json += "\"temperature_valid\":" + String(lastTemperatureValid ? "true" : "false") + ",";
    json += "\"max_temperature_c\":" + String(maxTemperature, 1) + ",";
    json += "\"overheat_latched\":" + String(overheatLatched ? "true" : "false") + ",";
    json += "\"mdns_enabled\":" + String(mdnsEnabled ? "true" : "false") + ",";
    json += "\"time_synced\":" + String(timeSynced ? "true" : "false");
    json += "}";
    return json;
}

static bool mqttPublishDiscoveryConfig(const String &component, const String &objectId, const String &payload) {
    String topic = mqttDiscoveryPrefix + "/" + component + "/" + mqttDeviceId + "/" + objectId + "/config";
    return mqttClient.publish(topic.c_str(), payload.c_str(), true);
}


static bool mqttStatePublishPending = false;

static bool mqttPublishState(bool force) {
    // RTSP/UI/callback paths only request publication. Coalesce their updates
    // and send the current state after audio has progressed in the main loop.
    if (force) { mqttStatePublishPending = true; return true; }
    if (!mqttClient.connected()) return false;
    unsigned long now = millis();
    unsigned long intervalMs = (unsigned long)mqttPublishIntervalSec * 1000UL;
    if (!mqttStatePublishPending && (now - lastMqttPublishMs) < intervalMs) return true;
    String topic = mqttStateTopic();
    String payload = mqttBuildStateJson();
    bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), false);
    if (ok) {
        mqttStatePublishPending = false;
        lastMqttPublishMs = now;
        if (mqttLastError == "state_publish_failed") {
            mqttLastError = mqttDiscoveryProgress.failed ? "discovery_publish_failed" : "ok";
        }
    }
    return ok;
}

static String mqttDiscoveryCommandTopic(mqtt_discovery::Command command) {
    using mqtt_discovery::Command;
    switch (command) {
    case Command::Rtsp: return mqttCmdRtspTopic();
    case Command::Reboot: return mqttCmdRebootTopic();
    case Command::Stream1Enabled: return mqttCmdStreamEnabledTopic(0);
    case Command::Stream2Enabled: return mqttCmdStreamEnabledTopic(1);
    case Command::Stream1Target: return mqttCmdStreamTargetTopic(0);
    case Command::Stream2Target: return mqttCmdStreamTargetTopic(1);
    default: return String();
    }
}

// Keep legacy cleanup, retained messages and one-message-per-step pacing.
static bool mqttPublishDiscoveryStep(uint16_t step) {
    constexpr uint16_t count = mqtt_discovery::entityCount;
    static_assert(count * 2 + 2 == DiscoveryProgress::total, "Discovery step count mismatch");
    if (step >= DiscoveryProgress::total) return false;
    if (step < count) {
        const auto& entity = mqtt_discovery::entities[step];
        if (mqttDeviceId == "esp32mic_000000000000") return true;
        String topic = mqttDiscoveryPrefix + "/" + entity.component + "/esp32mic_000000000000/" + entity.objectId + "/config";
        return mqttClient.publish(topic.c_str(), "", true);
    }
    if (step < count + 2) {
        String topic = step == count ? String("esp32mic/esp32mic_000000000000/availability")
                                    : String("esp32mic/") + mqttDeviceId + "/availability";
        if (topic == mqttAvailabilityTopic()) return true;
        return mqttClient.publish(topic.c_str(), "", true);
    }
    const auto& entity = mqtt_discovery::entities[step - count - 2];
    String payload = mqtt_discovery::makePayload(entity, mqttDeviceId, mqttStateTopic(),
        mqttAvailabilityTopic(), mqttBuildDeviceJson(), mqttDiscoveryCommandTopic(entity.command));
    return mqttPublishDiscoveryConfig(entity.component, entity.objectId, payload);
}

static void mqttScheduleDiscovery() {
    mqttDiscoveryPublished = false;
    mqttForceDiscovery = true;
    mqttDiscoveryProgress.reset();
}

static void mqttServiceDiscovery() {
    if (!mqttClient.connected() || !mqttForceDiscovery) return;
    if (!mqttDiscoveryProgress.ready(millis())) return;
    bool ok = mqttPublishDiscoveryStep(mqttDiscoveryProgress.step);
    mqttDiscoveryProgress.completeStep(ok, millis());
    if (!ok) mqttLastError = "discovery_publish_failed";
    if (mqttDiscoveryProgress.done()) {
        mqttDiscoveryPublished = true;
        mqttForceDiscovery = false;
        if (mqttLastError == "discovery_publish_failed") mqttLastError = "ok";
    }
}

static void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
    String t = topic ? String(topic) : String("");
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; ++i) msg += (char)payload[i];
    msg.trim();
    String up = msg;
    up.toUpperCase();

    if (t == mqttCmdRtspTopic()) {
        if (up == "ON") {
            if (overheatLatched || recording::mode()) {
                simplePrintln("MQTT command ignored: RTSP ON blocked by thermal protection or Recording mode.");
            } else if (!rtspServerEnabled) {
                rtspServer.begin();
                rtspServer.setNoDelay(true);
                rtspServerEnabled = true;
                simplePrintln("MQTT command: RTSP server enabled.");
            }
        } else if (up == "OFF") {
            cancelThermalResume();
            rtspServerEnabled = false;
            stopAllRtspClients("MQTT RTSP server disabled");
            rtspServer.stop();
            simplePrintln("MQTT command: RTSP server disabled.");
        }
        mqttPublishState(true);
        return;
    }

    for (uint8_t i = 0; i < 2; i++) {
        if (t == mqttCmdStreamEnabledTopic(i)) {
            if (up == "ON" || up == "OFF") {
                bool enabled = (up == "ON");
                if (streamEnabled[i] != enabled) {
                    streamEnabled[i] = enabled;
                    if (!enabled) stopRtspClientsForStream(i, "MQTT stream disabled");
                    saveAudioSettings();
                    simplePrintln("MQTT command: stream" + String(i + 1) + (enabled ? " enabled." : " disabled."));
                }
                mqttPublishState(true);
            }
            return;
        }

        if (t == mqttCmdStreamTargetTopic(i)) {
            uint8_t target = streamProfiles[i].target;
            if (up == "BIRDNET-GO" || up == "GO" || up == "0") {
                target = STREAM_TARGET_BIRDNET_GO;
            } else if (up == "BIRDNET-PI" || up == "PI" || up == "1") {
                target = STREAM_TARGET_BIRDNET_PI;
            } else {
                return;
            }
            if (streamProfiles[i].target != target) {
                streamProfiles[i].target = target;
                stopRtspClientsForStream(i, "MQTT stream target changed");
                saveAudioSettings();
                simplePrintln("MQTT command: stream" + String(i + 1) + " target set to " + streamTargetName(target) + ".");
            }
            mqttPublishState(true);
            return;
        }
    }

    if (t == mqttCmdRebootTopic()) {
        if (up == "PRESS" || up == "REBOOT") {
            simplePrintln("MQTT command: reboot requested.");
            scheduleReboot(false, 600);
        }
        return;
    }
}

static void mqttApplyClientSettings(bool logResult) {
    mqttNormalizeSettings();
    mqttClient.setServer(mqttHost.c_str(), mqttPort);
    mqttClient.setCallback(mqttMessageCallback);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);
    mqttClient.setBufferSize(1536);
    if (logResult) {
        simplePrintln("MQTT config: " + String(mqttEnabled ? "enabled" : "disabled") +
                      ", host=" + (mqttHost.length() ? mqttHost : String("(empty)")) +
                      ", port=" + String(mqttPort) +
                      ", topic=" + mqttTopicPrefix +
                      ", discovery=" + mqttDiscoveryPrefix +
                      ", clientId=" + mqttClientId);
    }
}

static bool mqttConnectNow(bool streamingReconnect = false) {
    mqttApplyClientSettings(false);
    if (!mqttEnabled) {
        mqttConnected = false;
        mqttLastError = "disabled";
        return false;
    }
    if (mqttHost.length() == 0) {
        mqttConnected = false;
        mqttLastError = "missing_host";
        return false;
    }

    if (streamingReconnect) {
        IPAddress brokerIp;
        bool haveBrokerIp = brokerIp.fromString(mqttHost);
        if (!haveBrokerIp && mqttResolvedIpValid && mqttResolvedHost == mqttHost) {
            brokerIp = mqttResolvedIp;
            haveBrokerIp = true;
        }
        if (!haveBrokerIp) {
            mqttConnected = false;
            mqttLastError = "reconnect_waiting_for_dns";
            return false;
        }

        // Keep a failed broker probe short enough that RTSP delivery can resume
        // promptly. PubSubClient will reuse this established TCP connection.
        mqttNetClient.stop();
        if (!mqttNetClient.connect(brokerIp, mqttPort, MQTT_STREAMING_CONNECT_TIMEOUT_MS)) {
            mqttConnected = false;
            mqttLastError = "connect_failed_tcp";
            return false;
        }
    }

    String availTopic = mqttAvailabilityTopic();
    bool ok = false;
    if (mqttUser.length()) {
        ok = mqttClient.connect(mqttClientId.c_str(), mqttUser.c_str(), mqttPassword.c_str(), availTopic.c_str(), 0, true, "offline");
    } else {
        ok = mqttClient.connect(mqttClientId.c_str(), availTopic.c_str(), 0, true, "offline");
    }
    if (!ok) {
        mqttConnected = false;
        mqttLastError = String("connect_failed_") + String(mqttClient.state());
        return false;
    }

    mqttConnected = true;
    mqttLastError = "ok";
    mqttResolvedIp = mqttNetClient.remoteIP();
    mqttResolvedHost = mqttHost;
    mqttResolvedIpValid = true;
    mqttClient.publish(availTopic.c_str(), "online", true);
    mqttClient.subscribe(mqttCmdRtspTopic().c_str());
    for (uint8_t i = 0; i < 2; i++) {
        mqttClient.subscribe(mqttCmdStreamEnabledTopic(i).c_str());
        mqttClient.subscribe(mqttCmdStreamTargetTopic(i).c_str());
    }
    mqttClient.subscribe(mqttCmdRebootTopic().c_str());
    mqttScheduleDiscovery();
    mqttPublishState(true);
    simplePrintln("MQTT connected to " + mqttHost + ":" + String(mqttPort));
    return true;
}

void mqttRequestReconnect(bool forceDiscovery) {
    (void)forceDiscovery;
    mqttScheduleDiscovery();
    lastMqttReconnectAttempt = 0;
    lastMqttPublishMs = 0;
    if (mqttClient.connected()) {
        String availTopic = mqttAvailabilityTopic();
        mqttClient.publish(availTopic.c_str(), "offline", true);
        mqttClient.disconnect();
    }
    mqttConnected = false;
}

void mqttPublishDiscoverySoon() {
    mqttScheduleDiscovery();
    if (!mqttClient.connected()) mqttRequestReconnect(true);
}

void checkMqtt() {
    if (devicesetup::wifiSuspended()) return;
    if (!mqttEnabled) {
        if (mqttClient.connected()) {
            String availTopic = mqttAvailabilityTopic();
            mqttClient.publish(availTopic.c_str(), "offline", true);
            mqttClient.disconnect();
        }
        mqttConnected = false;
        mqttLastError = "disabled";
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (mqttClient.connected()) mqttClient.disconnect();
        mqttConnected = false;
        mqttLastError = "wifi_disconnected";
        return;
    }

    if (!mqttClient.connected()) {
        unsigned long now = millis();
        // Streaming reconnects use a cached/numeric broker IP, a short TCP
        // timeout, and a deliberately relaxed retry cadence to protect audio.
        unsigned long interval = isStreaming
            ? MQTT_STREAMING_RECONNECT_INTERVAL_MS
            : MQTT_RECONNECT_INTERVAL_MS;
        if (lastMqttReconnectAttempt != 0 && (now - lastMqttReconnectAttempt) < interval) return;
        lastMqttReconnectAttempt = now;
        bool ok = mqttConnectNow(isStreaming);
        if (!ok && (now - lastMqttLogMs) > 60000UL) {
            simplePrintln("MQTT connect failed: " + mqttLastError);
            lastMqttLogMs = now;
        }
        return;
    }

    mqttConnected = true;
    mqttClient.loop();
    mqttServiceDiscovery();
    if (!mqttClient.connected()) { mqttConnected = false; return; }
    if (!mqttPublishState(false)) {
        mqttLastError = "state_publish_failed";
    }
}

uint16_t maxHighpassCutoffForRate(uint32_t sampleRate) {
    uint32_t maxCutoff = (uint32_t)((float)sampleRate * 0.45f);
    if (maxCutoff < 10U) maxCutoff = 10U;
    if (maxCutoff > 10000U) maxCutoff = 10000U;
    return (uint16_t)maxCutoff;
}

static uint16_t normalizeHighpassCutoff(uint32_t sampleRate, uint32_t requestedCutoff) {
    if (requestedCutoff < 10U) return 10U;
    uint16_t maxCutoff = maxHighpassCutoffForRate(sampleRate);
    return requestedCutoff > maxCutoff ? maxCutoff : (uint16_t)requestedCutoff;
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass)
void updateHighpassCoeffs() {
    // A later HTTP edit must be noticed on the next block, even if it arrives
    // while coefficients are being computed on the other core.
    const bool enabled = highpassEnabled;
    const uint32_t rate = currentSampleRate;
    const uint16_t cutoff = normalizeHighpassCutoff(rate, highpassCutoffHz);
    if (enabled) {
        fxaudio::biquadSetHighpass(&hpf, rate, cutoff);
    } else {
        fxaudio::biquadReset(&hpf);
    }
    hpfConfigEnabled = enabled;
    hpfConfigSampleRate = rate;
    hpfConfigCutoff = cutoff;
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

static const char* wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID";
        case WL_SCAN_COMPLETED:  return "SCAN_DONE";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

static String buildRtspDiag(WiFiClient &client) {
    unsigned long nowMs = millis();
    unsigned long idleMs = nowMs - lastRTSPActivity;
    String diag = "idle=" + String(idleMs) + "ms";
    diag += ", lastCmd=" + lastRtspCommand;
    if (lastRtspCommandMs > 0) {
        diag += " (" + String(nowMs - lastRtspCommandMs) + "ms ago)";
    } else {
        diag += " (never)";
    }
    if (lastStreamStopMs > 0) {
        diag += ", lastStop=" + lastStreamStopReason + " (" + String(nowMs - lastStreamStopMs) + "ms ago)";
    } else {
        diag += ", lastStop=none";
    }
    if (streamStartedAtMs > 0) {
        diag += ", streamAge=" + String(nowMs - streamStartedAtMs) + "ms";
    }
    if (lastRtpPacketMs > 0) {
        diag += ", rtpIdle=" + String(nowMs - lastRtpPacketMs) + "ms";
    }
    diag += ", packets=" + String(audioPacketsSent);
    diag += ", wifi=" + String(wifiStatusToString(WiFi.status()));
    diag += ", rssi=" + String(WiFi.RSSI()) + "dBm";
    if (client && client.connected()) {
        diag += ", client=" + client.remoteIP().toString();
    } else {
        diag += ", client=" + lastRtspClientIp;
    }
    return diag;
}

// Return true if we have a plausible current time (epoch after 2023-01-01)
static bool hasValidTime() {
    time_t now = time(nullptr);
    return now > 1672531200; // 2023-01-01 00:00:00 UTC
}

static void onNtpTimeSync(struct timeval* value) {
    if (value && value->tv_sec > 1672531200) ntpSyncEvents.fetch_add(1);
}

// Apply local time offset and optionally enable/disable NTP source servers.
// With enableNtp=false we keep local offset handling, but no network sync is configured.
void configureTimeService(bool enableNtp) {
    if (!ntpConfigured || ntpServiceEnabled != enableNtp) {
        esp_sntp_set_time_sync_notification_cb(onNtpTimeSync);
        if (enableNtp) {
            ntpSyncEvents.store(0);
            configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
            esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_SYNCED_MS);
        } else {
            // At an Off boot, configTime has not initialized the TCP/IP service yet.
            if (ntpServiceEnabled && esp_sntp_enabled()) esp_sntp_stop();
            ntpSyncEvents.store(0);
        }
        ntpConfigured = true;
        ntpServiceEnabled = enableNtp;
        ntpAttempted = ntpWasConnected = ntpReconnectPending = false;
    }
    const char* rule = timeZoneRule(timeZoneMode);
    if (timeZoneMode == TIME_ZONE_CUSTOM) rule = customTimeZoneRule.c_str();
    char fixedRule[24];
    if (!rule) {
        // POSIX TZ signs are opposite to UTC offsets. Keep minute precision
        // for existing API users and the Web UI's 15-minute increments.
        int32_t absolute = timeOffsetMinutes < 0 ? -timeOffsetMinutes : timeOffsetMinutes;
        snprintf(fixedRule, sizeof(fixedRule), "UTC%c%ld:%02ld",
                 timeOffsetMinutes >= 0 ? '-' : '+',
                 (long)(absolute / 60), (long)(absolute % 60));
        rule = fixedRule;
    }
    setenv("TZ", rule, 1);
    tzset();
}

bool isValidCustomTimeZoneRule(const String& rule) {
    return validCustomTimeZoneRule(rule.c_str());
}

int32_t effectiveTimeOffsetMinutes() {
    if (timeZoneMode == TIME_ZONE_FIXED || !hasValidTime()) return timeOffsetMinutes;
    time_t now = time(nullptr);
    struct tm local;
    struct tm utc;
    if (!localtime_r(&now, &local)) return timeOffsetMinutes;
    if (!gmtime_r(&now, &utc)) return timeOffsetMinutes;
    int dayDelta = local.tm_year == utc.tm_year
                       ? local.tm_yday - utc.tm_yday
                       : (local.tm_year > utc.tm_year ? 1 : -1);
    return dayDelta * 1440 + (local.tm_hour - utc.tm_hour) * 60 +
           local.tm_min - utc.tm_min;
}

static String formatClockHHMM(uint16_t mins) {
    mins %= 1440;
    uint8_t hh = (uint8_t)(mins / 60);
    uint8_t mm = (uint8_t)(mins % 60);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", hh, mm);
    return String(buf);
}

// Return true when current local time falls inside the [start, stop) window.
// If start == stop, window is treated as empty (always blocked).
static bool isScheduleWindowActive(uint16_t nowMin, uint16_t startMin, uint16_t stopMin) {
    if (startMin == stopMin) return false;
    if (startMin < stopMin) return (nowMin >= startMin && nowMin < stopMin);
    return (nowMin >= startMin || nowMin < stopMin); // overnight window
}

// Schedule policy: when local time is not valid, fail-open (do not block RTSP).
bool isStreamScheduleAllowedNow(bool* timeValidOut = nullptr) {
    bool validTime = hasValidTime();
    if (timeValidOut) *timeValidOut = validTime;
    if (!streamScheduleEnabled) return true;
    // Equal start/stop is an explicit empty window (always blocked), independent of time sync.
    if (streamScheduleStartMin == streamScheduleStopMin) return false;
    if (!validTime) return true;

    time_t now = time(nullptr);
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) return true; // fail-open on conversion issue

    uint16_t nowMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
    return isScheduleWindowActive(nowMin, streamScheduleStartMin, streamScheduleStopMin);
}

static void recordDeepSleepSnapshot(uint32_t sleepSec, uint32_t untilStartSec, const struct tm& tmNow) {
    rtcSleepPlannedSec = sleepSec;
    rtcSleepUntilStartSec = untilStartSec;
    rtcSleepStartMin = streamScheduleStartMin;
    rtcSleepStopMin = streamScheduleStopMin;
    rtcSleepEnteredMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
    rtcSleepOffsetMin = effectiveTimeOffsetMinutes();
    rtcSleepCycleCount++;
    rtcSleepSnapshotMagic = DEEP_SLEEP_SNAPSHOT_MAGIC;
}

static void logDeepSleepWakeSnapshotIfAny() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return;
    if (rtcSleepSnapshotMagic != DEEP_SLEEP_SNAPSHOT_MAGIC) {
        simplePrintln("Deep sleep wake: timer wake detected (no retained snapshot).");
        return;
    }
    simplePrintln("Deep sleep wake: cycle #" + String(rtcSleepCycleCount) +
                  ", planned " + String(rtcSleepPlannedSec) + " s, entered " +
                  formatClockHHMM(rtcSleepEnteredMin) + ", schedule " +
                  formatClockHHMM(rtcSleepStartMin) + "-" + formatClockHHMM(rtcSleepStopMin) +
                  ", until start at sleep " + String(rtcSleepUntilStartSec) +
                  " s, offset " + String(rtcSleepOffsetMin) + " min.");
    // Consume snapshot after first successful boot log to avoid repeated replay.
    rtcSleepSnapshotMagic = 0;
}

void checkStreamSchedule() {
    if (recording::mode()) return;
    if (!streamScheduleEnabled) {
        lastScheduleAllow = true;
        lastScheduleTimeValid = hasValidTime();
        return;
    }

    bool timeValid = false;
    bool allowNow = isStreamScheduleAllowedNow(&timeValid);
    bool invalidWindow = (streamScheduleStartMin == streamScheduleStopMin);
    unsigned long nowMs = millis();
    bool transitioned = (allowNow != lastScheduleAllow) || (timeValid != lastScheduleTimeValid);

    if (!timeValid) {
        if (lastScheduleUnsyncedLog == 0 || (nowMs - lastScheduleUnsyncedLog) > 600000UL) {
            simplePrintln("Stream schedule: local time unavailable, fail-open mode (RTSP allowed).");
            lastScheduleUnsyncedLog = nowMs;
        }
    }

    if (!allowNow) {
        stopAllRtspClients("Stream schedule window closed");
        if (rtspServerEnabled) {
            rtspServerEnabled = false;
            rtspServer.stop();
        }
        if (transitioned) {
            if (invalidWindow) {
                simplePrintln("Stream schedule: invalid/empty window (" +
                              formatClockHHMM(streamScheduleStartMin) + "-" +
                              formatClockHHMM(streamScheduleStopMin) +
                              "). RTSP server paused.");
            } else {
                simplePrintln("Stream schedule: outside allowed window " +
                              formatClockHHMM(streamScheduleStartMin) + "-" +
                              formatClockHHMM(streamScheduleStopMin) +
                              ". RTSP server paused.");
            }
            mqttPublishState(true);
        }
    } else if (transitioned) {
        if (overheatLatched) {
            simplePrintln("Stream schedule: window opened, but thermal latch keeps RTSP paused.");
        } else if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
            simplePrintln("Stream schedule: allowed window " +
                          formatClockHHMM(streamScheduleStartMin) + "-" +
                          formatClockHHMM(streamScheduleStopMin) +
                          ". RTSP server resumed.");
        }
        mqttPublishState(true);
    }

    lastScheduleAllow = allowNow;
    lastScheduleTimeValid = timeValid;
}

void checkDeepSleepSchedule() {
    if (recording::mode()) { deepSleepStatusCode = "recording_mode"; return; }
    deepSleepNextSleepSec = 0;

    if (!deepSleepScheduleEnabled) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "disabled";
        return;
    }
    if (!streamScheduleEnabled) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "schedule_disabled";
        return;
    }
    if (streamScheduleStartMin == streamScheduleStopMin) {
        // Invalid/empty schedule window: avoid accidental deep-sleep-only mode.
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "schedule_invalid";
        return;
    }

    bool timeValid = false;
    bool allowNow = isStreamScheduleAllowedNow(&timeValid);
    if (!timeValid) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "time_invalid";
        return;
    }
    if (allowNow) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "inside_window";
        return;
    }
    if (scheduledRebootAt != 0) {
        deepSleepStatusCode = "reboot_pending";
        return;
    }
    if (getRtspClientCount() > 0) {
        deepSleepStatusCode = "client_connected";
        return;
    }
    if (isStreaming) {
        deepSleepStatusCode = "streaming_active";
        return;
    }
    if (millis() < DEEP_SLEEP_BOOT_GRACE_MS) {
        deepSleepStatusCode = "grace_boot";
        return;
    }

    unsigned long nowMs = millis();
    if (deepSleepOutsideSinceMs == 0) deepSleepOutsideSinceMs = nowMs;
    if ((nowMs - deepSleepOutsideSinceMs) < DEEP_SLEEP_OUTSIDE_STABLE_MS) {
        deepSleepStatusCode = "outside_stabilizing";
        return;
    }

    time_t now = time(nullptr);
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) {
        deepSleepStatusCode = "time_invalid";
        return;
    }

    uint32_t untilStartSec = secondsUntilNextLocalMinute(now, streamScheduleStartMin);
    if (untilStartSec == 0) {
        deepSleepStatusCode = "time_invalid";
        return;
    }
    deepSleepNextSleepSec = untilStartSec;
    // If the next stream window is soon, stay awake and avoid edge flapping near boundary.
    if (untilStartSec <= (DEEP_SLEEP_MIN_SEC + DEEP_SLEEP_DRIFT_GUARD_SEC + 15UL)) {
        deepSleepStatusCode = "next_window_soon";
        return;
    }

    // Sleep only part of the remaining time so wake-up happens before the next window starts.
    uint32_t targetSleepSec = untilStartSec - DEEP_SLEEP_DRIFT_GUARD_SEC;
    uint32_t sleepSec = (targetSleepSec > DEEP_SLEEP_MAX_SEC)
                            ? DEEP_SLEEP_MAX_SEC
                            : targetSleepSec;
    if (sleepSec < DEEP_SLEEP_MIN_SEC) {
        deepSleepStatusCode = "next_window_soon";
        return;
    }

    deepSleepStatusCode = "ready";
    deepSleepNextSleepSec = sleepSec;
    recordDeepSleepSnapshot(sleepSec, untilStartSec, tmNow);

    simplePrintln("Deep sleep schedule: outside allowed stream window " +
                  formatClockHHMM(streamScheduleStartMin) + "-" +
                  formatClockHHMM(streamScheduleStopMin) +
                  ", sleeping for " + String(sleepSec) +
                  " s (wake guard " + String(DEEP_SLEEP_DRIFT_GUARD_SEC) + " s).");
    stopAllRtspClients("Deep sleep outside stream schedule");
    if (rtspServerEnabled) {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    if (mqttClient.connected()) {
        mqttPublishState(true);
        mqttClient.publish(mqttAvailabilityTopic().c_str(), "offline", true);
        mqttClient.loop();
        mqttClient.disconnect();
        delay(200);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
    Serial.flush();
    delay(30);
    esp_deep_sleep_start();
}

// Format current local date/time (uses NTP + offset), fallback to uptime
String formatDateTime() {
    time_t now = time(nullptr);
    if (hasValidTime()) {
        struct tm tmNow;
        localtime_r(&now, &tmNow);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
        return String(buf);
    }
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    return String("uptime ") + formatUptime(uptimeSeconds);
}

// Timestamp for log lines (short)
String formatLogTimestamp() {
    time_t now = time(nullptr);
    if (hasValidTime()) {
        struct tm tmNow;
        localtime_r(&now, &tmNow);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
        return String(buf);
    }
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    return String("uptime ") + formatUptime(uptimeSeconds);
}

// Queue an NTP request. Accepted does not mean synchronized: only the callback
// confirms that a server replied. Keep this signature for existing UI callers.
bool attemptTimeSync(bool logResult, bool quickMode /*prefer short, single shot*/ = false) {
    (void)quickMode;
    if (!timeSyncEnabled || WiFi.status() != WL_CONNECTED) {
        if (logResult) simplePrintln("NTP waiting for enabled Time Sync and WiFi connection");
        return false;
    }
    configureTimeService(true);
    const uint32_t now = millis();
    if (ntpAttempted && uint32_t(now-lastTimeSyncAttempt) < NTP_MIN_REQUEST_INTERVAL_MS) return true;
    const bool accepted = esp_sntp_restart();
    lastTimeSyncAttempt = now;
    ntpAttempted = true;
    ntpWasConnected = true;
    ntpReconnectPending = false;
    if (logResult) simplePrintln(accepted ? "NTP synchronization requested" : "NTP request failed; retry scheduled");
    return accepted;
}

const char* timeSyncStatusCode() {
    if (!timeSyncEnabled) return "disabled";
    if (timeSynced) return "synced";
    if (WiFi.status() != WL_CONNECTED) return "waiting_wifi";
    return "syncing";
}

// Run every application-loop pass: asynchronous responses can arrive after any
// request, including while streaming or before the first 30-second WiFi check.
void checkTimeSync() {
    if (devicesetup::wifiSuspended()) return;
    if (ntpSyncEvents.exchange(0) && timeSyncEnabled && hasValidTime()) {
        timeSynced = true;
        lastTimeSyncSuccess = millis();
        simplePrintln("Time synchronized via NTP");
    }
    if (!hasValidTime()) timeSynced = false;
    const bool connected = timeSyncEnabled && WiFi.status() == WL_CONNECTED;
    if (connected && !ntpWasConnected) ntpReconnectPending = true;
    ntpWasConnected = connected;
    if (!connected) return;
    const uint32_t elapsed = uint32_t(millis()-lastTimeSyncAttempt);
    const uint32_t interval = timeSynced ? NTP_SYNC_INTERVAL_SYNCED_MS : NTP_SYNC_INTERVAL_UNSYNCED_MS;
    if (!ntpAttempted || (ntpReconnectPending && elapsed >= NTP_MIN_REQUEST_INTERVAL_MS) || elapsed >= interval)
        attemptTimeSync(false, true);
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
    thermalRecovery.state.latched = overheatLatched;
    audioPrefs.putBytes("ohRecovery", &thermalRecovery.state, sizeof(thermalRecovery.state));
    audioPrefs.end();
}

static uint32_t thermalEpochNow() {
    return hasValidTime() ? (uint32_t)time(nullptr) : 0;
}

void cancelThermalResume() {
    recording::cancelResume();
    if (!thermalRecovery.state.resumePending) return;
    thermalRecovery.state.resumePending = 0;
    persistOverheatNote();
}

bool acknowledgeThermalShutdown() {
    if (otaInProgress) return false;
    bool wasLatched = overheatLatched;
    const ThermalRecoveryState saved = thermalRecovery.state;
    if (!thermalRecovery.acknowledge(millis(), overheatProtectionEnabled)) return false;
    overheatLatched = false;
    if (wasLatched && !setup_i2s_driver()) {
        thermalRecovery.state = saved;
        overheatLatched = true;
        simplePrintln("Thermal recovery: audio restart failed; shutdown retained.");
        return false;
    }
    overheatLockoutActive = false;
    overheatConsecutiveSamples = 0;
    // Preserve the last shutdown details for diagnosis after acknowledgement.
    persistOverheatNote();
    if (wasLatched && !recording::mode() && isStreamScheduleAllowedNow()) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    }
    if (wasLatched && recording::mode() && recording::resumePending()) resumeRecordingAfterCooling();
    mqttPublishState(true);
    return true;
}

void recordOverheatTrip(float temp) {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    overheatTripTemp = temp;
    overheatTriggeredAt = millis();
    overheatLastTimestamp = formatUptime(uptimeSeconds);
    overheatLastReason = String("Thermal shutdown: ") + String(temp, 1) + " C reached (limit " +
                         String(overheatShutdownC, 1) + " C).";
    thermalRecovery.trip(thermalEpochNow(), esp_timer_get_time() / 1000000ULL,
                         overheatShutdownC, temp >= overheatShutdownC + OVERHEAT_EMERGENCY_MARGIN_C,
                         rtspServerEnabled || (recording::mode() && audioProducerRunning));
    overheatLatched = true;
    simplePrintln("THERMAL PROTECTION: " + overheatLastReason);
    simplePrintln(thermalRecovery.autoPending(overheatAutoResumeEnabled, overheatProtectionEnabled)
                  ? "Waiting for five minutes at least 10 C below the limit before automatic recovery."
                  : "Manual acknowledgement required after cooling.");
    persistOverheatNote();
}

// Temperature monitoring + thermal protection
void checkTemperature() {
    float temp = readChipTemperature(); // Checked, range-aware internal sensor.
    bool tempValid = isTemperatureValid(temp);
    if (thermalRecovery.updateHistory(thermalEpochNow(), esp_timer_get_time() / 1000000ULL)) {
        persistOverheatNote();
    }
    thermalRecovery.sample(millis(), tempValid, temp);
    if (!tempValid) {
        overheatConsecutiveSamples = 0;
        lastTemperatureValid = false;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal recovery blocked.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;
    if (overheatSensorFault) {
        overheatSensorFault = false;
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }

    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    if (overheatLatched && !thermalRecovery.state.emergency &&
        temp >= overheatShutdownC + OVERHEAT_EMERGENCY_MARGIN_C) {
        // Further heating during a shutdown escalates the existing event,
        // rather than granting an automatic restart after a severe excursion.
        thermalRecovery.state.emergency = thermalRecovery.state.manualBlocked = 1;
        persistOverheatNote();
        simplePrintln("THERMAL PROTECTION: emergency temperature while paused; manual acknowledgement required.");
    }
    bool protectionActive = overheatProtectionEnabled && !overheatSensorFault;
    if (protectionActive) {
        if (!overheatLatched && (rtspServerEnabled || (recording::mode() && audioProducerRunning))) {
            bool confirmedTrip = false;
            if (temp >= (overheatShutdownC + OVERHEAT_EMERGENCY_MARGIN_C)) {
                // A clearly dangerous excursion still shuts down immediately.
                overheatConsecutiveSamples = 0;
                confirmedTrip = true;
            } else if (temp >= overheatShutdownC) {
                if (overheatConsecutiveSamples < OVERHEAT_CONFIRM_SAMPLES) {
                    overheatConsecutiveSamples++;
                }
                confirmedTrip = (overheatConsecutiveSamples >= OVERHEAT_CONFIRM_SAMPLES);
            } else {
                overheatConsecutiveSamples = 0;
            }

            if (confirmedTrip) {
                overheatLockoutActive = true;
                recordOverheatTrip(temp);
                // A persistent latch gates manual and automatic recovery.
                stopAllRtspClients("Thermal protection");
                rtspServerEnabled = false;
                rtspServer.stop();
                recording::pauseForCooling();
                pauseAudioCapture();
                mqttPublishState(true);
            }
        }
    } else {
        overheatConsecutiveSamples = 0;
    }
    if (thermalRecovery.canResume(millis(), overheatAutoResumeEnabled,
                                  overheatProtectionEnabled, recording::mode() || isStreamScheduleAllowedNow())) {
        overheatLatched = false;
        if (!setup_i2s_driver()) {
            overheatLatched = true;
            thermalRecovery.resetCooling();
            simplePrintln("Thermal recovery: audio restart failed; shutdown retained.");
            return;
        }
        thermalRecovery.resume();
        overheatLockoutActive = false;
        overheatConsecutiveSamples = 0;
        persistOverheatNote();
        if (!recording::mode()) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
        } else if (recording::resumePending()) {
            resumeRecordingAfterCooling();
        }
        simplePrintln("THERMAL RECOVERY: Audio restored after stable cooling. Warning awaits acknowledgement.");
        mqttPublishState(true);
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
        uint32_t currentRate = getCurrentPacketRate(255);

        if (currentRate > maxPacketRate) maxPacketRate = currentRate;
        if (currentRate < minPacketRate) minPacketRate = currentRate;

        if (currentRate < minAcceptableRate) {
            simplePrintln("Stream output below configured minimum.");
        }
    }
}

void checkAudioProducerHealth() {
    // Read the cross-task timestamp before the clock to avoid treating a newer
    // producer timestamp as an unsigned negative age.
    uint32_t lastRead = audioProducerLastReadMs;
    uint32_t now = millis();
    if (recording::busy() && (!audioProducerRunning ||
        uint32_t(now - lastRead) > 2000 ||
        getI2sDmaOverrunTotal() != recordingDmaBaseline)) recording::captureFault();
    if (autoRecoveryEnabled && isStreaming &&
        audioProducerNeedsRecovery(now, lastRead, audioProducerStopRequested, lastI2SReset)) {
        lastI2SReset = now;
        simplePrintln("AUTO-RECOVERY: Audio capture stopped; restarting I2S.");
        restartI2S();
    }
}

// WiFi health check
void checkWiFiHealth() {
    if (devicesetup::wifiSuspended()) return;
    static wl_status_t lastStatus = WL_IDLE_STATUS;
    static bool initialized = false;
    wl_status_t cur = WiFi.status();
    if (!initialized) {
        lastStatus = cur;
        initialized = true;
    }
    if (cur == WL_CONNECTED && lastStatus != WL_CONNECTED) {
        wifiReconnectCount++;
        simplePrintln("WiFi reconnected: " + WiFi.localIP().toString() +
                      " (count " + String(wifiReconnectCount) + ")");
        applyMdnsSetting();
        if (timeSyncEnabled) {
            attemptTimeSync(false);
        }
        mqttRequestReconnect(true);
        mqttPublishState(true);
    }
    lastStatus = cur;

    // Re-apply TX power WITHOUT logging (prevent periodic log spam)
    applyWifiTxPower(false);

    int32_t rssi = WiFi.RSSI();
    if (rssi < -85) {
        simplePrintln("WARNING: Weak WiFi signal: " + String(rssi) + " dBm");
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled || recording::mode()) return;

    unsigned long uptimeHours = (millis() - bootTime) / 3600000;
    if (uptimeHours >= resetIntervalHours) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        delay(1000);
        ESP.restart();
    }
}

// Load only time settings early so the very first boot logs use the persisted local offset.
static void preloadTimeSettingsForEarlyLogs() {
    Preferences bootPrefs;
    if (bootPrefs.begin("audio", true)) {
        timeOffsetMinutes = bootPrefs.getInt("timeOffset", 0);
        if (timeOffsetMinutes < -720 || timeOffsetMinutes > 840) timeOffsetMinutes = 0;
        timeZoneMode = bootPrefs.getUChar("timeZone", TIME_ZONE_FIXED);
        customTimeZoneRule = bootPrefs.getString("timeZoneRule", "UTC0");
        if (timeZoneMode > TIME_ZONE_CUSTOM ||
            (timeZoneMode == TIME_ZONE_CUSTOM && !isValidCustomTimeZoneRule(customTimeZoneRule)))
            timeZoneMode = TIME_ZONE_FIXED;
        timeSyncEnabled = bootPrefs.getBool("timeSyncEn", true);
        bootPrefs.end();
    }
    configureTimeService(timeSyncEnabled);
}

// Load settings from flash
void loadAudioSettings() {
    bool settingsRepaired = false;
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // (1) respect compile-time default 12 on first boot
    i2sShiftBits = audioPrefs.getUChar("shiftBits", i2sShiftBits);
    micFormat = audioPrefs.getUChar("micFormat", DEFAULT_MIC_FORMAT);
    s3BoardVariant = audioPrefs.getUChar("s3Board", s3boards::External);
    if (!s3BoardSelectionSupported() || !s3boards::valid(s3BoardVariant)) {
        if (s3BoardVariant != s3boards::External) settingsRepaired = true;
        s3BoardVariant = s3boards::External;
    }
    updateS3BoardIdentity();
    if (!micBoardFormatAllowed(micFormat)) {
        micFormat = MIC_FORMAT_PDM;
        settingsRepaired = true;
    }
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
    overheatAutoResumeEnabled = audioPrefs.getBool("ohAuto", false);
    timeOffsetMinutes = audioPrefs.getInt("timeOffset", 0);
    timeZoneMode = audioPrefs.getUChar("timeZone", TIME_ZONE_FIXED);
    customTimeZoneRule = audioPrefs.getString("timeZoneRule", "UTC0");
    timeSyncEnabled = audioPrefs.getBool("timeSyncEn", true);
    mdnsEnabled = audioPrefs.getBool("mdnsEn", true);
    mdnsHostname = sanitizeMdnsHostname(audioPrefs.getString("mdnsHost", defaultMdnsHostname()), defaultMdnsHostname());
    streamScheduleEnabled = audioPrefs.getBool("strSchedEn", false);
    streamScheduleStartMin = (uint16_t)audioPrefs.getUInt("strSchStart", 0);
    streamScheduleStopMin = (uint16_t)audioPrefs.getUInt("strSchStop", 0);
    deepSleepScheduleEnabled = audioPrefs.getBool("deepSchSlp", false);
    mqttEnabled = audioPrefs.getBool("mqttEn", false);
    mqttHost = audioPrefs.getString("mqttHost", "");
    uint32_t storedMqttPort = audioPrefs.getUInt("mqttPort", DEFAULT_MQTT_PORT);
    mqttPort = (storedMqttPort >= 1U && storedMqttPort <= 65535U)
                   ? (uint16_t)storedMqttPort
                   : DEFAULT_MQTT_PORT;
    if (storedMqttPort != mqttPort) settingsRepaired = true;
    mqttUser = audioPrefs.getString("mqttUser", "");
    mqttPassword = audioPrefs.getString("mqttPass", "");
    mqttTopicPrefix = audioPrefs.getString("mqttTop", "");
    mqttDiscoveryPrefix = audioPrefs.getString("mqttDisc", "homeassistant");
    mqttClientId = audioPrefs.getString("mqttCid", "");
    uint32_t storedMqttInterval = audioPrefs.getUInt("mqttIntSec", DEFAULT_MQTT_PUBLISH_INTERVAL_SEC);
    mqttPublishIntervalSec = (storedMqttInterval >= MQTT_PUBLISH_INTERVAL_MIN_SEC &&
                              storedMqttInterval <= MQTT_PUBLISH_INTERVAL_MAX_SEC)
                                 ? (uint16_t)storedMqttInterval
                                 : DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
    if (storedMqttInterval != mqttPublishIntervalSec) settingsRepaired = true;
    streamProfiles[0].target = (uint8_t)audioPrefs.getUChar("s1Target", STREAM_TARGET_BIRDNET_GO);
    streamProfiles[1].target = (uint8_t)audioPrefs.getUChar("s2Target", STREAM_TARGET_BIRDNET_PI);
    streamEnabled[0] = audioPrefs.getBool("s1Enabled", true);
    streamEnabled[1] = audioPrefs.getBool("s2Enabled", false);
    maxActiveClients = (uint8_t)audioPrefs.getUChar("maxClients", 2);
    if (streamScheduleStartMin > 1439) { streamScheduleStartMin = 0; settingsRepaired = true; }
    if (streamScheduleStopMin > 1439) { streamScheduleStopMin = 0; settingsRepaired = true; }
    uint32_t ohLimit = audioPrefs.getUInt("ohThresh", DEFAULT_OVERHEAT_LIMIT_C);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    ohLimit = OVERHEAT_MIN_LIMIT_C + ((ohLimit - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C) * OVERHEAT_LIMIT_STEP_C;
    overheatShutdownC = (float)ohLimit;
    overheatLastReason = audioPrefs.getString("ohReason", "");
    overheatLastTimestamp = audioPrefs.getString("ohStamp", "");
    overheatTripTemp = audioPrefs.getFloat("ohTripC", 0.0f);
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    ThermalRecoveryState storedThermal;
    storedThermal.magic = 0;
    if (audioPrefs.getBytesLength("ohRecovery") == sizeof(storedThermal)) {
        audioPrefs.getBytes("ohRecovery", &storedThermal, sizeof(storedThermal));
    }
    thermalRecovery.restore(storedThermal, overheatLatched, esp_timer_get_time() / 1000000ULL, overheatShutdownC);
    overheatLatched = thermalRecovery.state.latched;
    overheatLockoutActive = overheatLatched;
    audioPrefs.end();

    if (currentSampleRate < 8000U || currentSampleRate > 192000U) {
        currentSampleRate = DEFAULT_SAMPLE_RATE;
        settingsRepaired = true;
    }
    if (!isfinite(currentGainFactor) || currentGainFactor < 0.1f || currentGainFactor > 100.0f) {
        currentGainFactor = DEFAULT_GAIN_FACTOR;
        settingsRepaired = true;
    }
    if (currentBufferSize < 256U || currentBufferSize > 8192U) {
        currentBufferSize = DEFAULT_BUFFER_SIZE;
        settingsRepaired = true;
    }
    if (i2sShiftBits > 24U) {
        i2sShiftBits = 12;
        settingsRepaired = true;
    }
    if (micFormat > MIC_FORMAT_PDM || (micFormat == MIC_FORMAT_PDM && !pdmMicSupported())) {
        micFormat = DEFAULT_MIC_FORMAT;
        settingsRepaired = true;
    }
    if (micFormat == MIC_FORMAT_PDM && currentSampleRate != PDM_SAMPLE_RATE) {
        currentSampleRate = PDM_SAMPLE_RATE;
        settingsRepaired = true;
    }
    if (resetIntervalHours < 1U || resetIntervalHours > 168U) {
        resetIntervalHours = 24;
        settingsRepaired = true;
    }
    if (minAcceptableRate < 5U || minAcceptableRate > 200U) {
        minAcceptableRate = 50;
        settingsRepaired = true;
    }
    if (performanceCheckInterval < 1U || performanceCheckInterval > 60U) {
        performanceCheckInterval = 15;
        settingsRepaired = true;
    }
    if (!cpuFrequencySupported(cpuFrequencyMhz)) {
        cpuFrequencyMhz = 160;
        settingsRepaired = true;
    }
    if (!isfinite(wifiTxPowerDbm) || wifiTxPowerDbm < -1.0f || wifiTxPowerDbm > 19.5f) {
        wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
        settingsRepaired = true;
    } else {
        float snappedTx = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
        if (fabsf(snappedTx - wifiTxPowerDbm) > 0.01f) settingsRepaired = true;
        wifiTxPowerDbm = snappedTx;
    }
    uint16_t normalizedCutoff = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (normalizedCutoff != highpassCutoffHz) {
        highpassCutoffHz = normalizedCutoff;
        settingsRepaired = true;
    }
    if (timeOffsetMinutes < -720 || timeOffsetMinutes > 840) {
        timeOffsetMinutes = 0;
        settingsRepaired = true;
    }
    if (timeZoneMode > TIME_ZONE_CUSTOM ||
        (timeZoneMode == TIME_ZONE_CUSTOM && !isValidCustomTimeZoneRule(customTimeZoneRule))) {
        timeZoneMode = TIME_ZONE_FIXED;
        settingsRepaired = true;
    }
    if (streamProfiles[0].target > STREAM_TARGET_BIRDNET_PI) { streamProfiles[0].target = STREAM_TARGET_BIRDNET_GO; settingsRepaired = true; }
    if (streamProfiles[1].target > STREAM_TARGET_BIRDNET_PI) { streamProfiles[1].target = STREAM_TARGET_BIRDNET_PI; settingsRepaired = true; }
    if (maxActiveClients < 1 || maxActiveClients > MAX_CLIENTS) { maxActiveClients = 2; settingsRepaired = true; }
    mqttApplyClientSettings(false);

    // Apply timezone/offset immediately after loading persisted settings so that
    // *all* early boot logs (including "Loaded settings") use the correct local time.
    // This does not block; NTP sync is attempted later when Wi-Fi is available.
    configureTimeService(timeSyncEnabled);

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (settingsRepaired) {
        simplePrintln("Invalid stored settings repaired and replaced with safe values.");
        saveAudioSettings();
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
                  ", micFormat=" + String(micFormatName(micFormat)) +
                  ", HPF=" + String(highpassEnabled?"on":"off") +
                  ", HPFcut=" + String(highpassCutoffHz) + "Hz");
}

// Save settings to flash
void saveAudioSettings() {
    mqttApplyClientSettings(false);
    audioPrefs.begin("audio", false);
    audioPrefs.putUInt("sampleRate", currentSampleRate);
    audioPrefs.putFloat("gainFactor", currentGainFactor);
    audioPrefs.putUShort("bufferSize", currentBufferSize);
    audioPrefs.putUChar("shiftBits", i2sShiftBits);
    audioPrefs.putUChar("micFormat", micFormat);
    audioPrefs.putUChar("s3Board", s3BoardVariant);
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
    audioPrefs.putBool("ohAuto", overheatAutoResumeEnabled);
    uint32_t ohLimit = (uint32_t)(overheatShutdownC + 0.5f);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    audioPrefs.putUInt("ohThresh", ohLimit);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    thermalRecovery.state.latched = overheatLatched;
    audioPrefs.putBytes("ohRecovery", &thermalRecovery.state, sizeof(thermalRecovery.state));
    audioPrefs.putInt("timeOffset", timeOffsetMinutes);
    audioPrefs.putUChar("timeZone", timeZoneMode);
    audioPrefs.putString("timeZoneRule", customTimeZoneRule);
    audioPrefs.putBool("timeSyncEn", timeSyncEnabled);
    audioPrefs.putBool("mdnsEn", mdnsEnabled);
    audioPrefs.putString("mdnsHost", mdnsHostname);
    audioPrefs.putBool("strSchedEn", streamScheduleEnabled);
    audioPrefs.putUInt("strSchStart", streamScheduleStartMin);
    audioPrefs.putUInt("strSchStop", streamScheduleStopMin);
    audioPrefs.putBool("deepSchSlp", deepSleepScheduleEnabled);
    audioPrefs.putBool("mqttEn", mqttEnabled);
    audioPrefs.putString("mqttHost", mqttHost);
    audioPrefs.putUInt("mqttPort", (uint32_t)mqttPort);
    audioPrefs.putString("mqttUser", mqttUser);
    audioPrefs.putString("mqttPass", mqttPassword);
    audioPrefs.putString("mqttTop", mqttTopicPrefix);
    audioPrefs.putString("mqttDisc", mqttDiscoveryPrefix);
    audioPrefs.putString("mqttCid", mqttClientId);
    audioPrefs.putUInt("mqttIntSec", (uint32_t)mqttPublishIntervalSec);
    audioPrefs.putUChar("s1Target", streamProfiles[0].target);
    audioPrefs.putUChar("s2Target", streamProfiles[1].target);
    audioPrefs.putBool("s1Enabled", streamEnabled[0]);
    audioPrefs.putBool("s2Enabled", streamEnabled[1]);
    audioPrefs.putUChar("maxClients", maxActiveClients);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// mDNS management
void applyMdnsSetting() {
    if (!mdnsEnabled) {
        if (mdnsRunning) {
            MDNS.end();
            mdnsRunning = false;
            simplePrintln("mDNS disabled");
        }
        return;
    }
    if (mdnsRunning) return;
    if (!MDNS.begin(mdnsHostname.c_str())) {
        simplePrintln("mDNS start failed");
        mdnsRunning = false;
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("rtsp", "tcp", 8554);
    mdnsRunning = true;
    simplePrintln("mDNS ready: http://" + mdnsHostname + ".local/ ; rtsp://" + mdnsHostname + ".local:8554/audio1");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    if (!recording::stop()) return;
    recording::cancelResume();
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Schedule a WiFi reconnect after delayMs, optionally pinning to a specific BSSID
void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs) {
    if (wifiApPinned()) bssid = wifiApPinned();
    if (bssid) {
        wifiReconnectHasBssid = true;
        memcpy(wifiReconnectBssid, bssid, 6);
    } else {
        wifiReconnectHasBssid = false;
    }
    wifiReconnectAt = millis() + delayMs;
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
    antenna::save(true);
    devicesetup::reset(true);
    battery::resetCalibration();
    fieldpower::saveSettings(fieldpower::Settings{});
    recording::saveSettings(recording::Settings{});
    recording::saveLogSettings(recording::LogSettings{});
    simplePrintln("FACTORY RESET: Restoring default settings...");
    wifiApSave(nullptr);
    clearStoredBssidPin();

    // Clear persisted settings in our namespace
    audioPrefs.begin("audio", false);
    audioPrefs.clear();
    audioPrefs.end();

    // Reset runtime variables to defaults
    currentSampleRate = DEFAULT_SAMPLE_RATE;
    currentGainFactor = DEFAULT_GAIN_FACTOR;
    currentBufferSize = DEFAULT_BUFFER_SIZE;
    i2sShiftBits = 12;  // compile-time default respected
    micFormat = DEFAULT_MIC_FORMAT;
    s3BoardVariant = s3boards::External;
    updateS3BoardIdentity();

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
    overheatAutoResumeEnabled = false;
    thermalRecovery = ThermalRecovery{};
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLockoutActive = false;
    overheatConsecutiveSamples = 0;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;
    timeOffsetMinutes = 0;
    timeZoneMode = TIME_ZONE_FIXED;
    customTimeZoneRule = "UTC0";
    timeSyncEnabled = true;
    mdnsEnabled = true;
    mdnsHostname = defaultMdnsHostname();
    streamScheduleEnabled = false;
    streamScheduleStartMin = 0;
    streamScheduleStopMin = 0;
    deepSleepScheduleEnabled = false;
    deepSleepOutsideSinceMs = 0;
    deepSleepStatusCode = "disabled";
    deepSleepNextSleepSec = 0;
    timeSynced = false;
    lastTimeSyncAttempt = 0;
    lastTimeSyncSuccess = 0;
    mqttEnabled = false;
    mqttHost = "";
    mqttPort = DEFAULT_MQTT_PORT;
    mqttUser = "";
    mqttPassword = "";
    mqttTopicPrefix = mqttDefaultTopicPrefix();
    mqttDiscoveryPrefix = "homeassistant";
    mqttClientId = mqttDefaultClientId();
    mqttPublishIntervalSec = DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
    mqttConnected = false;
    mqttLastError = "disabled";
    mqttDiscoveryPublished = false;
    mqttForceDiscovery = false;

    isStreaming = false;
    streamEnabled[0] = true;
    streamEnabled[1] = false;
    maxActiveClients = 2;
    streamProfiles[0].target = STREAM_TARGET_BIRDNET_GO;
    streamProfiles[1].target = STREAM_TARGET_BIRDNET_PI;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Apply all producer-visible audio settings only after the producer has stopped.
// This prevents a larger requested buffer size from being used with the old,
// smaller allocation during a Web UI reconfiguration.
bool applyAudioConfig(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift) {
    if (micFormat == MIC_FORMAT_PDM && newRate != PDM_SAMPLE_RATE) return false;
    uint32_t oldRate = currentSampleRate;
    float oldGain = currentGainFactor;
    uint16_t oldBuffer = currentBufferSize;
    uint8_t oldShift = i2sShiftBits;
    uint16_t oldCutoff = highpassCutoffHz;
    uint32_t oldMinRate = minAcceptableRate;

    stopAudioProducer();
    currentSampleRate = newRate;
    currentGainFactor = newGain;
    currentBufferSize = newBuffer;
    i2sShiftBits = newShift;
    highpassCutoffHz = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();

    if (restartI2S()) return true;

    simplePrintln("Audio setting rejected: I2S restart failed, rolling back.");
    currentSampleRate = oldRate;
    currentGainFactor = oldGain;
    currentBufferSize = oldBuffer;
    i2sShiftBits = oldShift;
    highpassCutoffHz = oldCutoff;
    minAcceptableRate = oldMinRate;
    if (!restartI2S()) {
        simplePrintln("Audio rollback failed; reboot recommended.");
    }
    return false;
}

// Stop the producer BEFORE changing format/rate: it must not decode old DMA
// bytes with the new sample width. Roll back the complete dependent state.
bool applyMicFormatConfig(uint8_t newFormat) {
    if (!micBoardFormatAllowed(newFormat)) return false;
    if (newFormat > MIC_FORMAT_PDM || (newFormat == MIC_FORMAT_PDM && !pdmMicSupported())) return false;
    if (newFormat == micFormat) return true;
    uint8_t oldFormat = micFormat;
    uint32_t oldRate = currentSampleRate;
    uint16_t oldCutoff = highpassCutoffHz;
    uint32_t oldMinRate = minAcceptableRate;
    stopAudioProducer();
    micFormat = newFormat;
    if (newFormat == MIC_FORMAT_PDM) currentSampleRate = PDM_SAMPLE_RATE;
    highpassCutoffHz = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();
    if (restartI2S()) return true;
    simplePrintln("Mic format rejected; restoring previous audio configuration.");
    micFormat = oldFormat;
    currentSampleRate = oldRate;
    highpassCutoffHz = oldCutoff;
    minAcceptableRate = oldMinRate;
    if (!restartI2S()) simplePrintln("Mic format rollback failed; reboot recommended.");
    return false;
}

// Apply microphone and its optional profile as one capture transaction.
// The legacy API continues to preserve custom values through applyMicFormatConfig.
bool applyMicProfileConfig(uint8_t newFormat, bool recommended) {
    if (!micBoardFormatAllowed(newFormat)) return false;
    if (!recommended) return applyMicFormatConfig(newFormat);
    micprofiles::Settings next;
    if (!getRecommendedMicSettings(newFormat, next) ||
        (newFormat == MIC_FORMAT_PDM && !pdmMicSupported())) return false;
    const uint8_t oldFormat = micFormat, oldCpu = cpuFrequencyMhz;
    const micprofiles::Settings old = {currentSampleRate, currentGainFactor, currentBufferSize,
        i2sShiftBits, highpassEnabled, highpassCutoffHz, cpuFrequencyMhz};
    const uint32_t oldMinRate = minAcceptableRate;
    stopAudioProducer();
    micFormat = newFormat;
    currentSampleRate = next.rate;
    currentGainFactor = next.gain;
    currentBufferSize = next.buffer;
    if (newFormat != MIC_FORMAT_PDM) i2sShiftBits = next.shift;
    highpassEnabled = next.highpass;
    highpassCutoffHz = next.cutoff;
    if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();
    const uint8_t nextCpu = next.cpu ? next.cpu : oldCpu;
    const bool clockOk = nextCpu == oldCpu || setCpuFrequencyMhz(nextCpu);
    if (clockOk) cpuFrequencyMhz = nextCpu;
    if (clockOk && restartI2S()) return true;

    micFormat = oldFormat;
    currentSampleRate = old.rate;
    currentGainFactor = old.gain;
    currentBufferSize = old.buffer;
    i2sShiftBits = old.shift;
    highpassEnabled = old.highpass;
    highpassCutoffHz = old.cutoff;
    minAcceptableRate = oldMinRate;
    if (cpuFrequencyMhz != oldCpu && !setCpuFrequencyMhz(oldCpu))
        simplePrintln("Microphone profile CPU rollback failed.");
    cpuFrequencyMhz = oldCpu;
    if (!restartI2S()) simplePrintln("Microphone profile rollback failed; reboot recommended.");
    return false;
}

// Stop capture before changing GPIO routing, even when the format stays PDM.
// Persist only after success in the authenticated API handler.
s3boards::Config getS3BoardConfig() {
    return {s3BoardVariant, micFormat, currentSampleRate, minAcceptableRate, highpassCutoffHz};
}

bool restoreS3BoardConfig(const s3boards::Config& saved) {
    if (recording::busy()) return false;
    stopAudioProducer();
    s3BoardVariant = saved.variant;
    micFormat = saved.format;
    currentSampleRate = saved.rate;
    minAcceptableRate = saved.minRate;
    highpassCutoffHz = saved.cutoff;
    updateS3BoardIdentity();
    return restartI2S();
}

bool applyS3BoardConfig(uint8_t variant) {
    if (!s3BoardSelectionSupported() || !s3boards::valid(variant) || recording::busy()) return false;
    if (recording::mode() && variant == s3boards::AtomS3U) return false;
    if (variant == s3BoardVariant) return true;
    const uint8_t oldVariant = s3BoardVariant, oldFormat = micFormat;
    const uint32_t oldRate = currentSampleRate, oldMinRate = minAcceptableRate;
    const uint16_t oldCutoff = highpassCutoffHz;
    stopAudioProducer();
    s3BoardVariant = variant;
    if (s3boards::builtin(variant)) {
        micFormat = MIC_FORMAT_PDM;
        currentSampleRate = PDM_SAMPLE_RATE;
    }
    highpassCutoffHz = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();
    if (restartI2S()) {
        updateS3BoardIdentity();
        return true;
    }
    s3BoardVariant = oldVariant;
    micFormat = oldFormat;
    currentSampleRate = oldRate;
    highpassCutoffHz = oldCutoff;
    minAcceptableRate = oldMinRate;
    if (!restartI2S()) simplePrintln("S3 board rollback failed; reboot recommended.");
    return false;
}

// Capture and RTP work buffers are touched for every packet: keep them internal,
// since with PSRAM in the heap plain malloc() prefers PSRAM above 4 KB.
static inline void* audioWorkAlloc(size_t bytes) {
#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_SPIRAM
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

bool recordingHardwareSupported() {
    return recordingpins::resolve(s3BoardVariant).supported;
}

bool startRecording() {
    if (!fieldpower::canStart(battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant))) return false;
    if (recording::busy() || otaInProgress || flashlayout::busy() || overheatLatched ||
        !audioProducerRunning || audioProducerStopRequested || scheduledRebootAt) return false;
    recordingDmaBaseline = getI2sDmaOverrunTotal();
    return recording::start(currentSampleRate, recordingHardwareSupported(), s3BoardVariant);
}

bool resumeRecordingAfterCooling() {
    if (!fieldpower::canStart(battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant))) {
        recording::cancelResume(); return false;
    }
    if (recording::busy() || otaInProgress || flashlayout::busy() || overheatLatched ||
        !audioProducerRunning || audioProducerStopRequested || scheduledRebootAt) {
        recording::cancelResume();
        return false;
    }
    recordingDmaBaseline = getI2sDmaOverrunTotal();
    return recording::resume(currentSampleRate, recordingHardwareSupported(), s3BoardVariant);
}

bool setDeviceMode(bool recordingMode) {
    if (recordingMode && !recordingHardwareSupported()) return false;
    if (recordingMode == recording::mode()) return true;
    if (otaInProgress || flashlayout::busy()) return false;
    const bool wasRecording = recording::busy();
    if (!recording::stop() || (wasRecording && recording::status().state == recording::State::Error)) return false;
    recordingAutoStartPending = false;
    if (!recordingMode) cancelThermalResume();
    if (!webui_logModeChanged(recordingMode)) return false;
    recording::setRuntimeMode(recordingMode);
    devicesetup::currentMode(recordingMode);
    browserAudioStop();
    stopAllRtspClients("Device mode changed");
    rtspServerEnabled = false;
    rtspServer.stop();
    if (recordingMode) recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    if (!recordingMode && !overheatLatched && isStreamScheduleAllowedNow()) {
        rtspServer.begin(); rtspServer.setNoDelay(true); rtspServerEnabled = true;
    }
    return true;
}

// Restart I2S with new parameters
bool restartI2S() {
    if (recording::busy()) return false;
    lastI2SReset = millis();
    simplePrintln("Restarting I2S with new parameters...");
    stopAllRtspClients("I2S restart");
    stopAudioProducer();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
    if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
    if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
    if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }

    i2s_32bit_buffer = (int32_t*)audioWorkAlloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)audioWorkAlloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer || !i2s_16bit_network_buffer || !rtpPacketScratch) {
        simplePrintln("FATAL: Memory allocation failed after parameter change!");
        if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
        if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
        if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
        if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }
        return false;
    }

    if (!setup_i2s_driver()) {
        simplePrintln("FATAL: I2S restart failed!");
        return false;
    }
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
    return true;
}

// Minimal print helpers: Serial + buffered for Web UI
void simplePrint(String message) {
    Serial.print(message);
}

void simplePrintln(String message) {
    String line = "[" + formatLogTimestamp() + "] " + message;
    Serial.println(line);
    webui_pushLog(line);
}

// OTA setup
void setupOTA() {
    String preparationError;
    if (!flashlayout::otaAllowed(preparationError)) {
        simplePrintln("ArduinoOTA unavailable: " + preparationError);
        return;
    }
    // Keep OTA hostname aligned with our mDNS hostname to avoid multiple .local names
    ArduinoOTA.setHostname(mdnsHostname.c_str());
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.onStart([] { beginOtaUpdate(); });
    ArduinoOTA.onError([](ota_error_t) { endOtaUpdate(false); });
    ArduinoOTA.begin();
}

static bool hasActiveRtspStream() {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) return true;
    }
    return false;
}

static size_t computeAudioRingBufferCapacity() {
    size_t chunkBytes = (size_t)currentBufferSize * sizeof(int16_t);
    size_t chunkCount = 8;
    if (currentBufferSize > 4096) chunkCount = 3;
    else if (currentBufferSize > 2048) chunkCount = 4;
    size_t capacity = chunkBytes * chunkCount;
    // Keep RTP packets small for UDP compatibility while providing roughly
    // 64 chunks (~683 ms at 48 kHz/512 samples) of internal TCP jitter reserve.
    // Extra reserve is drained on recovery, rather than adding a fixed delay.
    if (currentBufferSize <= 2048 && capacity < 65536) capacity = 65536;
    if (capacity < 4096) capacity = 4096;
    return capacity;
}

void flushAudioRingBuffer() {
    if (!audioRingBuffer) return;
    size_t itemSize = 0;
    void* item = nullptr;
    uint32_t flushed = 0;
    while ((item = xRingbufferReceive(audioRingBuffer, &itemSize, 0)) != nullptr) {
        vRingbufferReturnItem(audioRingBuffer, item);
        flushed++;
    }
    if (flushed > 0) {
        audioRingBufferFlushCount += flushed;
    }
}

// Keep the overflow ISR bounded: no allocation, logging, clock calls or DSP.
static bool IRAM_ATTR onI2sDmaOverrun(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&audioDmaMux);
    if (audioDmaCountingEnabled) ++audioDmaOverrunTotal;
    portEXIT_CRITICAL_ISR(&audioDmaMux);
    return false;
}

static void setI2sDmaCounting(bool enabled) {
    portENTER_CRITICAL(&audioDmaMux);
    audioDmaCountingEnabled = enabled && !audioProducerStopRequested;
    portEXIT_CRITICAL(&audioDmaMux);
}

static uint32_t getI2sDmaOverrunTotal() {
    portENTER_CRITICAL(&audioDmaMux);
    uint32_t total = audioDmaOverrunTotal;
    portEXIT_CRITICAL(&audioDmaMux);
    return total;
}

static void registerI2sDiagnostics() {
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = onI2sDmaOverrun;
    esp_err_t result = i2s_channel_register_event_callback(i2s_rx_handle, &callbacks, nullptr);
    audioDmaCallbackAvailable = (result == ESP_OK);
    if (!audioDmaCallbackAvailable) {
        simplePrintln("I2S DMA diagnostics unavailable: " + String(esp_err_to_name(result)));
    }
}

static esp_err_t enableI2sReceiverWithDiagnostics() {
    registerI2sDiagnostics();
    esp_err_t result = i2s_channel_enable(i2s_rx_handle);
    if (result != ESP_OK) audioDmaCallbackAvailable = false;
    return result;
}

DmaOverrunStatus getI2sDmaStatus() {
    return {audioDmaCallbackAvailable, getI2sDmaOverrunTotal(), audioDmaWindow.count(),
            audioDmaWindow.hasEvent(), audioDmaWindow.lastEventMs()};
}

void checkMicrophoneSignal() {
    uint32_t now = millis();
    static MicrophoneSignal::State previousSignal = MicrophoneSignal::Unavailable;
    auto signal = microphoneSignal.state(now, audioProducerRunning && !audioProducerStopRequested);
    if (signal != previousSignal) {
        if (signal == MicrophoneSignal::Flatline)
            simplePrintln("Microphone signal unchanged for 20 s; check microphone wiring/power.");
        else if (signal == MicrophoneSignal::Healthy && previousSignal == MicrophoneSignal::Flatline)
            simplePrintln("Microphone signal restored.");
        previousSignal = signal;
        mqttPublishState(true);
    }
}

void updateAudioDiagnostics() {
#if MIC_PDM_SOFTWARE
    uint8_t stride = pdmStrideLogPending.exchange(0, std::memory_order_relaxed);
    if (stride) simplePrintln(stride == 2 ? "PDM raw rate: paired slots; using every second word."
                                        : "PDM raw rate: plain stream; using every word.");
#endif
    uint32_t now = millis();
    uint32_t added = audioDmaWindow.update(now, getI2sDmaOverrunTotal());
    static bool logged = false;
    static uint32_t lastLogMs = 0;
    if (added && (!logged || uint32_t(now - lastLogMs) >= 60000)) {
        logged = true;
        lastLogMs = now;
        simplePrintln("I2S DMA overrun: " + String(audioDmaWindow.count()) +
                      " observed in the last 60 s; capture could not keep up.");
    }
}

#if MIC_PDM_SOFTWARE
static int readPdmSoftwareBlock(uint64_t& filterUs) {
    int produced = 0;
    while (produced < (int)currentBufferSize && !audioProducerStopRequested) {
        size_t bytesRead = 0;
        size_t want = size_t(currentBufferSize - produced) * 8 * (pdmFramer.stride == 1 ? 1 : 2);
        if (want > sizeof(pdmReadBuffers->bytes)) want = sizeof(pdmReadBuffers->bytes);
        esp_err_t err = i2s_channel_read(i2s_rx_handle, pdmReadBuffers->bytes, want, &bytesRead, 100);
        if (audioProducerStopRequested) break;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) { audioI2SErrorCount++; recording::captureFault(); break; }
        if (!bytesRead) { audioI2SErrorCount++; recording::captureFault(); break; }
        audioProducerLastReadMs = millis();
        uint64_t started = esp_timer_get_time();
        size_t words = pdmFramer.unpack(pdmReadBuffers->bytes, bytesRead, pdmReadBuffers->words);
        uint8_t detected = pdmWordRate.observe(words, started,
            PDM_SAMPLE_RATE * pdmsw::kWordsPerSample, getI2sDmaOverrunTotal());
        if (detected) {
            if (pdmDetectedStride.exchange(detected, std::memory_order_relaxed) == 0)
                pdmStrideLogPending.store(detected, std::memory_order_relaxed);
            if (detected != pdmFramer.stride) {
                pdmFramer.stride = detected;
                pdmFramer.secondOfPair = false;
                pdmReadChunks = 0;
                pdmDetectedOrder.store(0, std::memory_order_relaxed);
                pdmsw::init(pdmDecimator, false);
                produced = 0; // discard samples decoded with the provisional stride
            }
        }
        size_t compacted = pdmFramer.compact(pdmReadBuffers->words, words);
        ++pdmReadChunks;
        if (detected && pdmDetectedOrder.load(std::memory_order_relaxed) == 0 &&
            pdmReadChunks >= 4 && (pdmReadChunks - 4) % 32 == 0 && compacted >= 256) {
            auto order = pdmsw::detectBitOrder(pdmDecimator, pdmReadBuffers->words, compacted);
            pdmDetectedOrder.store(uint8_t(order), std::memory_order_relaxed);
        }
        produced += int(pdmsw::process(pdmDecimator, pdmReadBuffers->words, compacted,
                                       i2s_32bit_buffer + produced, currentBufferSize - produced));
        filterUs += esp_timer_get_time() - started;
        if (err == ESP_ERR_TIMEOUT) break;
    }
    return produced;
}
#endif

void audioProducerTask(void* /*arg*/) {
    audioProducerRunning = true;
    setI2sDmaCounting(true);
    AudioProcessingWindow processingLoad;
    processingLoad.reset(esp_timer_get_time());

    const bool pdm = micFormat == MIC_FORMAT_PDM;
    const size_t sampleBytes = pdm && MIC_PDM_HARDWARE ? sizeof(int16_t) : sizeof(int32_t);
    while (!audioProducerStopRequested) {
        uint64_t filterUs = 0;
        int samplesRead = 0;
#if MIC_PDM_SOFTWARE
        if (pdm) samplesRead = readPdmSoftwareBlock(filterUs);
        else
#endif
        {
            size_t bytesRead = 0;
            esp_err_t result = i2s_channel_read(i2s_rx_handle, i2s_32bit_buffer,
                                                currentBufferSize * sampleBytes, &bytesRead, 100);
            if (audioProducerStopRequested) break;
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) { audioI2SErrorCount++; recording::captureFault(); continue; }
            samplesRead = bytesRead / sampleBytes;
            if (samplesRead == 0) { audioI2SErrorCount++; recording::captureFault(); vTaskDelay(pdMS_TO_TICKS(1)); continue; }
            audioProducerLastReadMs = millis();
        }
        if (audioProducerStopRequested) break;
        if (samplesRead <= 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        uint64_t processingStartedUs = esp_timer_get_time();

        // Hardware PDM is packed int16. Widen backwards without overwriting
        // unread samples; both PDM paths then share the same fixed PCM scale.
        if (pdm && MIC_PDM_HARDWARE) {
            const uint8_t* packed = (const uint8_t*)i2s_32bit_buffer;
            for (int i = samplesRead - 1; i >= 0; --i) {
                int16_t value;
                memcpy(&value, packed + size_t(i) * sizeof(value), sizeof(value));
                i2s_32bit_buffer[i] = int32_t(value) * 65536;
            }
        }
        if (hpfConfigEnabled != highpassEnabled || hpfConfigSampleRate != currentSampleRate ||
            hpfConfigCutoff != highpassCutoffHz) updateHighpassCoeffs();

        int32_t rawMinimum = INT32_MAX, rawMaximum = INT32_MIN;
        for (int i = 0; i < samplesRead; ++i) {
            int32_t raw = i2s_32bit_buffer[i];
            if (raw < rawMinimum) rawMinimum = raw;
            if (raw > rawMaximum) rawMaximum = raw;
        }
        bool clipped = false;
        // PDM retains its defined int16 PCM scale; I2S Shift stays I2S-only.
        lastPeakAbs16 = fxaudio::processBlock(i2s_32bit_buffer, i2s_16bit_buffer, samplesRead,
            pdm ? 16 : i2sShiftBits, hpfConfigEnabled ? &hpf : nullptr,
            fxaudio::gainToQ(currentGainFactor), &clipped);
        microphoneSignal.observe(rawMinimum, rawMaximum, millis());
        recording::push(i2s_16bit_buffer, samplesRead);
        browserAudioPush(i2s_16bit_buffer, samplesRead);
        audioClippedLastBlock = clipped;
        if (clipped) audioClipCount++;

        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = millis() + 3000UL;
        } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        if (audioRingBuffer && hasActiveRtspStream()) {
            size_t payloadBytes = (size_t)samplesRead * sizeof(int16_t);
            if (xRingbufferSend(audioRingBuffer, i2s_16bit_buffer, payloadBytes, 0) == pdTRUE) {
                audioRingBufferChunkCount++;
            } else {
                // Capture remains the only writer; keep the existing atomic counter.
                audioRingBufferDropCount.store(
                    audioRingBufferDropCount.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            }
        }
        uint64_t processingFinishedUs = esp_timer_get_time();
        if (processingLoad.add(processingFinishedUs, processingFinishedUs - processingStartedUs + filterUs)) {
            audioProcessingLoadSnapshot.store(processingLoad.snapshot(), std::memory_order_relaxed);
        }
    }

    setI2sDmaCounting(false);
    audioProcessingLoadSnapshot.store(0, std::memory_order_relaxed);
    audioProducerRunning = false;
    audioProducerTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

static void deleteAudioRingBuffer() {
    if (!audioRingBuffer) return;
#if AUDIO_RING_IN_PSRAM
    vRingbufferDeleteWithCaps(audioRingBuffer);
#else
    vRingbufferDelete(audioRingBuffer);
#endif
    audioRingBuffer = nullptr;
    audioRingInPsram = false;
}

#if AUDIO_RING_IN_PSRAM
// Leave 64 KB of PSRAM for other large allocations; otherwise use internal RAM.
static bool createPsramAudioRing(size_t requested) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // The ring itself goes to PSRAM, so only Wi-Fi headroom and the producer
    // stack are needed internally. Use the C5 restart allowance (64 KB) here
    // too: a client disconnected for the restart may still hold TCP buffers.
    if (!audioreserve::externalHeadroom(heap_caps_get_free_size(internal),
                                       heap_caps_get_largest_free_block(internal),
                                       audioreserve::kC5MinimumHeapMargin)) return false;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < requested + 64 * 1024) return false;
    audioRingBuffer = xRingbufferCreateWithCaps(requested, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audioRingBuffer) return false;
    audioRingInPsram = true;
    audioRingBufferCapacityBytes = requested;
    simplePrintln("Audio reserve: " + String((uint32_t)requested) + " bytes in PSRAM; internal free " +
                  String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                  " bytes; PSRAM free " + String((uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)) + " KB");
    return true;
}
#endif

void stopAudioProducer() {
    browserAudioStop();
    audioProducerStopRequested = true;
    setI2sDmaCounting(false);
    unsigned long startMs = millis();
    while (audioProducerTaskHandle != nullptr && (millis() - startMs) < 600UL) {
        delay(5);
    }
    if (audioProducerTaskHandle != nullptr) {
        vTaskDelete(audioProducerTaskHandle);
        audioProducerTaskHandle = nullptr;
        audioProducerRunning = false;
    }
    deleteAudioRingBuffer();
    audioRingBufferCapacityBytes = 0;
    audioRingBufferFlushPending = false;
    audioProcessingLoadSnapshot.store(0, std::memory_order_relaxed);
    microphoneSignal.reset();
    audioProducerStopRequested = false;
}

// Stop capture and its clocks as well as network listeners. Leaving the PDM
// converter running prevents cooling and competes with flash writes during OTA.
void pauseAudioCapture() {
    recording::requestStop();
    stopAudioProducer();
    recording::stop();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }
    audioDmaCallbackAvailable = false;
    releasePdmCapture();
}

static bool otaRestoreRtsp = false;

void beginOtaUpdate() {
    if (otaInProgress) return;
    otaRestoreRtsp = rtspServerEnabled;
    otaInProgress = true;
    stopAllRtspClients("OTA update starting");
    rtspServerEnabled = false;
    rtspServer.stop();
    pauseAudioCapture();
    overheatConsecutiveSamples = 0;
    simplePrintln("OTA: audio capture paused until reboot or update failure.");
}

void endOtaUpdate(bool installed) {
    if (!otaInProgress || installed) return; // Successful OTA stays paused until reboot.
    otaInProgress = false;
    if (overheatLatched) return;
    if (!setup_i2s_driver()) {
        simplePrintln("OTA failed: audio restart failed; reboot required.");
        return;
    }
    if (otaRestoreRtsp && !recording::mode() && isStreamScheduleAllowedNow()) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    }
    simplePrintln("OTA failed: audio capture restored.");
}

static RingbufHandle_t createInternalAudioRing(size_t bytes) {
#if AUDIO_RING_IN_PSRAM
    return xRingbufferCreateWithCaps(bytes, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xRingbufferCreate(bytes, RINGBUF_TYPE_BYTEBUF);
#endif
}

bool startAudioProducer() {
    stopAudioProducer();

    const size_t requested = computeAudioRingBufferCapacity();
#if AUDIO_RING_IN_PSRAM
    // PSRAM first; the unchanged internal-RAM reserve path below is the fallback.
    if (!createPsramAudioRing(requested)) {
        simplePrintln("Audio reserve: PSRAM unavailable, using internal RAM.");
#endif
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t heapFree = heap_caps_get_free_size(caps);
    const size_t heapLargest = heap_caps_get_largest_free_block(caps);
    const size_t chunkBytes = size_t(currentBufferSize) * sizeof(int16_t);
    audioRingBufferCapacityBytes = audioreserve::fit(requested,
        chunkBytes, heapFree, heapLargest);
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (!audioRingBufferCapacityBytes) {
        audioRingBufferCapacityBytes = audioreserve::fit(
            audioreserve::minimumCapacity(requested, chunkBytes), chunkBytes,
            heapFree, heapLargest, audioreserve::kC5MinimumHeapMargin);
        if (audioRingBufferCapacityBytes) {
            simplePrintln("Audio reserve: low RAM, using minimum reserve with 64 KiB Wi-Fi headroom.");
        }
    }
#endif
    simplePrintln("Audio reserve check: free " + String((uint32_t)heapFree) +
                  " bytes; largest block " + String((uint32_t)heapLargest));
    if (!audioRingBufferCapacityBytes) {
        simplePrintln("Audio reserve: insufficient RAM for Wi-Fi and capture.");
        return false;
    }
    audioRingBuffer = createInternalAudioRing(audioRingBufferCapacityBytes);
    if (!audioRingBuffer) {
        simplePrintln("Audio RB alloc failed");
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    simplePrintln("Audio reserve: " + String((uint32_t)audioRingBufferCapacityBytes) +
                  " bytes; requested " + String((uint32_t)requested));
#if AUDIO_RING_IN_PSRAM
    }
#endif
    updateHighpassCoeffs(); // producer is stopped here
    audioProducerStopRequested = false;
    audioProducerLastReadMs = millis();
    BaseType_t rc = xTaskCreate(
        audioProducerTask,
        "audio_prod",
        audioreserve::kProducerStackBytes,
        nullptr,
        5,
        &audioProducerTaskHandle
    );
    if (rc != pdPASS) {
        simplePrintln("Audio producer failed");
        deleteAudioRingBuffer();
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    simplePrintln("Audio producer started");
    return true;
}

// I2S setup
bool setup_i2s_driver() {
    stopAudioProducer();
    audioDmaCallbackAvailable = false;
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    releasePdmCapture();
    // A persisted thermal shutdown must boot with capture stopped so it can
    // actually cool. Settings changes must not bypass either pause condition.
    if (overheatLatched || otaInProgress) return true;
    if (micFormat == MIC_FORMAT_PDM && (!pdmMicSupported() || currentSampleRate != PDM_SAMPLE_RATE)) return false;
    uint16_t dma_buf_len = (currentBufferSize > 512) ? 512 : currentBufferSize;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = dma_buf_len;
#if MIC_PDM_SOFTWARE
    if (micFormat == MIC_FORMAT_PDM) chan_cfg.dma_frame_num = 1024;
#endif

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &i2s_rx_handle);
    if (err != ESP_OK) {
        simplePrintln("I2S channel create failed: " + String(esp_err_to_name(err)));
        return false;
    }

    if (micFormat == MIC_FORMAT_PDM) {
#if SOC_I2S_SUPPORTS_PDM_RX
        i2s_pdm_rx_config_t config = {};
#if SOC_I2S_SUPPORTS_PDM2PCM
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_SAMPLE_RATE);
        config.clk_cfg.dn_sample_mode = I2S_PDM_DSR_8S;
        config.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#else
        // Both are just over 4 KB; with PSRAM enabled, malloc() prefers PSRAM above
        // 4 KB. The filter reads them for every PDM word, so keep them internal.
        pdmDecimator = (pdmsw::Decimator*)audioWorkAlloc(sizeof(pdmsw::Decimator));
        pdmReadBuffers = (PdmReadBuffers*)audioWorkAlloc(sizeof(PdmReadBuffers));
        if (!pdmDecimator || !pdmReadBuffers) {
            releasePdmCapture();
            i2s_del_channel(i2s_rx_handle); i2s_rx_handle = nullptr;
            return false;
        }
        pdmsw::init(pdmDecimator, false);
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_CLOCK_HZ);
        config.slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
        config.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
        config.gpio_cfg.clk = (gpio_num_t)s3boards::clockPin(s3BoardVariant, I2S_BCLK_PIN);
        config.gpio_cfg.din = (gpio_num_t)s3boards::dataPin(s3BoardVariant, I2S_DOUT_PIN);
        err = i2s_channel_init_pdm_rx_mode(i2s_rx_handle, &config);
        if (err == ESP_OK) err = enableI2sReceiverWithDiagnostics();
        if (err == ESP_OK && startAudioProducer()) {
#if MIC_PDM_SOFTWARE
            simplePrintln(String("PDM filter buffers: ") +
                          (esp_ptr_external_ram(pdmDecimator) || esp_ptr_external_ram(pdmReadBuffers) ? "PSRAM" : "internal RAM"));
#endif
            simplePrintln(String("PDM ready: 48000 Hz, ") + pdmDecoderName() +
                          ", CLK=GPIO" + String(s3boards::clockPin(s3BoardVariant, I2S_BCLK_PIN)) +
                          ", DATA=GPIO" + String(s3boards::dataPin(s3BoardVariant, I2S_DOUT_PIN)) +
                          "; WS/MCLK unused.");
            return true;
        }
        audioDmaCallbackAvailable = false;
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle); i2s_rx_handle = nullptr;
        releasePdmCapture();
#endif
        simplePrintln("PDM setup failed; check configuration/memory.");
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(currentSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DOUT_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // PCM1808 slave mode accepts 256/384/512 fs. Mics without an MCLK input
    // leave D7 unconnected and continue to use the same BCLK/WS/SD wiring.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (micFormat == MIC_FORMAT_MSB) {
        std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }

    err = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
        simplePrintln("I2S pin/mode setup failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    err = enableI2sReceiverWithDiagnostics();
    if (err != ESP_OK) {
        audioDmaCallbackAvailable = false;
        simplePrintln("I2S channel enable failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits) +
                  ", micFormat " + String(micFormatName(micFormat)) +
                  ", pins MCLK/BCLK/WS/SD " +
                  String(I2S_MCLK_PIN) + "/" + String(I2S_BCLK_PIN) + "/" +
                  String(I2S_LRCLK_PIN) + "/" + String(I2S_DOUT_PIN));
    if (!startAudioProducer()) {
        audioDmaCallbackAvailable = false;
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }
    return true;
}

static bool anotherSessionStreaming(const ClientSession &session) {
    for (uint8_t i = 0; i < MAX_CLIENTS; ++i) {
        if (&clients[i] != &session && clients[i].streaming) return true;
    }
    return false;
}

static void recordRtpSent(ClientSession &session, uint16_t samples, uint32_t chunk) {
    session.rtpSequence++;
    session.rtpTimestamp += samples;
    session.packetsSent++;
    session.lastRtpPacketMs = millis();
    lastRtpPacketMs = session.lastRtpPacketMs;
    // A chunk delivered to multiple clients is counted once per profile.
    // Delayed copies from a slower client must not count the same chunk again.
    uint8_t pi = session.profileIndex;
    if ((int32_t)(chunk - countedProfileChunk[pi]) > 0) {
        countedProfileChunk[pi] = chunk;
        streamStats[pi].packetsSent++;
    }
    if ((int32_t)(chunk - countedAudioChunk) > 0) {
        countedAudioChunk = chunk;
        audioPacketsSent++;
    }
}

static void stopStreamOnWriteFailure(ClientSession &session, const char* reason) {
    uint8_t pi = session.profileIndex;
    session.streaming = false;
    rtspWriteFailCount++;
    recordStreamStop(reason, pi + 1);
    bool otherStreaming = anotherSessionStreaming(session);
    session.reset();
    // A slow/failed client must not discard audio queued for other clients.
    if (!otherStreaming) audioRingBufferFlushPending = true;
    bool anyStillStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming && clients[i].profileIndex == pi) { anyStillStreaming = true; break; }
    }
    if (!anyStillStreaming) streamStats[pi].streaming = false;

    simplePrintln("STREAMING STOPPED: " + lastStreamStopReason + " | client dropped");
    mqttPublishState(true);
}

static void servicePendingTcp(ClientSession &session) {
    if (!session.tcpWriter.pending()) return;
    uint32_t now = millis();
    bool wasStalled = session.tcpWriter.stalled();
    auto result = session.tcpWriter.poll(now, [&](const uint8_t* data, size_t length) {
        int fd = session.client.fd();
        if (!session.client.connected() || fd < 0) { errno = ENOTCONN; return -1; }
        return (int)send(fd, data, length, MSG_DONTWAIT);
    });
    if (!wasStalled && session.tcpWriter.stalled()) rtspWriteStallCount++;
    if (result == CooperativeTcpWriter::TIMED_OUT) rtspWriteTimeoutCount++;
    if (result == CooperativeTcpWriter::FAILED || result == CooperativeTcpWriter::TIMED_OUT) {
        stopStreamOnWriteFailure(session, session.tcpFrameIsRtp ? "RTP write failed" : "RTSP write failed");
        return;
    }
    if (result != CooperativeTcpWriter::COMPLETE) return;
    if (shouldFlushRecoveredBacklog(session.tcpWriter.stalled(), session.streaming,
                                    anotherSessionStreaming(session),
                                    session.tcpFrameDropsAtQueue,
                                    audioRingBufferDropCount.load(std::memory_order_relaxed))) {
        audioRingBufferFlushPending = true;
    }
    if (session.tcpFrameIsRtp) recordRtpSent(session, session.tcpFrameSamples, session.tcpFrameChunk);
    session.tcpWriter.clear();
    session.tcpFrameIsRtp = false;
}

static void queueRtspResponse(ClientSession &session, const String &response) {
    session.tcpFrameIsRtp = false;
    session.tcpFrameDropsAtQueue = audioRingBufferDropCount.load(std::memory_order_relaxed);
    if (!session.tcpWriter.queue((const uint8_t*)response.c_str(), response.length(), millis())) {
        stopStreamOnWriteFailure(session, "RTSP write failed");
        return;
    }
    servicePendingTcp(session);
}

void sendRTPPacket(ClientSession &session, const int16_t* networkAudioData, int numSamples, uint32_t chunk) {
    if (session.tcpWriter.pending()) return;
    const uint16_t payloadSize = (uint16_t)(numSamples * (int)sizeof(int16_t));
    const uint16_t packetSize = (uint16_t)(12 + payloadSize);

    uint8_t header[12];
    header[0] = 0x80;
    header[1] = 96;
    header[2] = (uint8_t)((session.rtpSequence >> 8) & 0xFF);
    header[3] = (uint8_t)(session.rtpSequence & 0xFF);
    header[4] = (uint8_t)((session.rtpTimestamp >> 24) & 0xFF);
    header[5] = (uint8_t)((session.rtpTimestamp >> 16) & 0xFF);
    header[6] = (uint8_t)((session.rtpTimestamp >> 8) & 0xFF);
    header[7] = (uint8_t)(session.rtpTimestamp & 0xFF);
    header[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
    header[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
    header[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
    header[11] = (uint8_t)(rtpSSRC & 0xFF);

    bool success = false;
    if (session.transport == TRANSPORT_TCP) {
        if (!session.client.connected()) {
            session.streaming = false;
            return;
        }
        if (!rtpPacketScratch) {
            stopStreamOnWriteFailure(session, "RTP write failed (no scratch buffer)");
            return;
        }
        rtpPacketScratch[0] = 0x24;
        rtpPacketScratch[1] = 0x00;
        rtpPacketScratch[2] = (uint8_t)((packetSize >> 8) & 0xFF);
        rtpPacketScratch[3] = (uint8_t)(packetSize & 0xFF);
        memcpy(rtpPacketScratch + 4, header, sizeof(header));
        memcpy(rtpPacketScratch + 4 + sizeof(header), networkAudioData, payloadSize);
        size_t totalLen = 4 + sizeof(header) + payloadSize;
        if (!session.tcpWriter.queue(rtpPacketScratch, totalLen, millis())) {
            stopStreamOnWriteFailure(session, "RTP write failed");
            return;
        }
        session.tcpFrameIsRtp = true;
        session.tcpFrameSamples = (uint16_t)numSamples;
        session.tcpFrameChunk = chunk;
        session.tcpFrameDropsAtQueue = audioRingBufferDropCount.load(std::memory_order_relaxed);
        servicePendingTcp(session);
        return;
    } else {
        if (session.clientRtpPort == 0) {
            session.streaming = false;
            return;
        }
        session.udpSocket.beginPacket(session.clientRtpAddress, session.clientRtpPort);
        session.udpSocket.write(header, sizeof(header));
        session.udpSocket.write((const uint8_t*)networkAudioData, payloadSize);
        if (session.udpSocket.endPacket()) {
            success = true;
        }
    }
    if (success) {
        recordRtpSent(session, (uint16_t)numSamples, chunk);
    }
}

static void drainRtcpPackets(ClientSession &session) {
    if (session.transport != TRANSPORT_UDP) return;
    uint8_t buffer[64];
    int packetSize = session.rtcpSocket.parsePacket();
    while (packetSize > 0) {
        while (packetSize > 0) {
            int toRead = packetSize;
            if (toRead > (int)sizeof(buffer)) toRead = sizeof(buffer);
            int readNow = session.rtcpSocket.read(buffer, toRead);
            if (readNow <= 0) break;
            packetSize -= readNow;
        }
        packetSize = session.rtcpSocket.parsePacket();
    }
}

// Audio streaming
void streamAudio() {
    bool anyStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) { anyStreaming = true; break; }
    }
    if (!anyStreaming) return;
    if (!audioRingBuffer) return;

    if (audioRingBufferFlushPending) {
        audioRingBufferFlushPending = false;
        flushAudioRingBuffer();
    }
    uint8_t chunksProcessed = 0;
    while (chunksProcessed < 4) {
        bool ready = false;
        for (uint8_t i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].streaming && !clients[i].tcpWriter.pending()) { ready = true; break; }
        }
        if (!ready) break;
        size_t itemSize = 0;
        // Byte buffers merge adjacent producer writes. Only borrow the bytes
        // this RTP packet can carry, so returning it cannot discard queued audio.
        int16_t* audioChunk = (int16_t*)xRingbufferReceiveUpTo(
            audioRingBuffer, &itemSize, 0,
            (size_t)currentBufferSize * sizeof(int16_t));
        if (!audioChunk) break;

        int samplesRead = itemSize / sizeof(int16_t);
        if (samplesRead > currentBufferSize) samplesRead = currentBufferSize;

        // Prepare one network-order copy and reuse for all active client sessions.
        for (int i = 0; i < samplesRead; ++i) {
            uint16_t s = (uint16_t)audioChunk[i];
            s = (uint16_t)((s << 8) | (s >> 8));
            i2s_16bit_network_buffer[i] = (int16_t)s;
        }
        uint32_t chunk = ++audioChunkSequence;
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].streaming || clients[i].tcpWriter.pending()) continue;
            sendRTPPacket(clients[i], i2s_16bit_network_buffer, samplesRead, chunk);
        }
        vRingbufferReturnItem(audioRingBuffer, (void*)audioChunk);
        if (audioRingBufferFlushPending) {
            audioRingBufferFlushPending = false;
            flushAudioRingBuffer();
            break;
        }
        chunksProcessed++;
    }
}

static uint8_t detectProfileFromRequest(const String &request) {
    int lineEnd = request.indexOf("\r\n");
    String firstLine = (lineEnd > 0) ? request.substring(0, lineEnd) : request;
    if (firstLine.indexOf("/audio2") >= 0) return 1;
    return 0;
}

static String rtspHeaderValue(const String &request, const char *headerName) {
    String marker = String(headerName) + ":";
    int start = request.indexOf(marker);
    if (start < 0) return String();
    start += marker.length();
    while (start < (int)request.length() && (request[start] == ' ' || request[start] == '\t')) start++;
    int end = request.indexOf("\r\n", start);
    if (end < 0) end = request.length();
    String value = request.substring(start, end);
    value.trim();
    return value;
}

static bool anyRtspSessionStreaming(uint8_t profileIndex = 255) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].streaming) continue;
        if (profileIndex == 255 || clients[i].profileIndex == profileIndex) return true;
    }
    return false;
}

static void parseTransportHeader(ClientSession &session, const String &request, String &transportResponse, uint8_t clientIdx) {
    // Auto-select transport based on target: BirdNET-Go=TCP, BirdNET-Pi=UDP
    bool forceTcp = (streamProfiles[session.profileIndex].target == STREAM_TARGET_BIRDNET_GO);
    bool forceUdp = !forceTcp;

    if (forceUdp) {
        session.transport = TRANSPORT_UDP;
        int transportPos = request.indexOf("Transport:");
        if (transportPos >= 0) {
            String transportLine = request.substring(transportPos);
            int endPos = transportLine.indexOf("\r\n");
            if (endPos > 0) transportLine = transportLine.substring(0, endPos);
            transportLine.toLowerCase();
            int clientPortPos = transportLine.indexOf("client_port=");
            if (clientPortPos >= 0) {
                String portStr = transportLine.substring(clientPortPos + 12);
                int dashPos = portStr.indexOf("-");
                if (dashPos > 0) portStr = portStr.substring(0, dashPos);
                int semiPos = portStr.indexOf(";");
                if (semiPos > 0) portStr = portStr.substring(0, semiPos);
                portStr.trim();
                session.clientRtpPort = (uint16_t)portStr.toInt();
            }
        }
        if (session.client.connected()) session.clientRtpAddress = session.client.remoteIP();
        session.serverRtpPort = (uint16_t)(5004 + (clientIdx * 2));
        session.udpSocket.stop();
        session.rtcpSocket.stop();
        session.udpSocket.begin(session.serverRtpPort);
        session.rtcpSocket.begin(session.serverRtpPort + 1);
        transportResponse = "RTP/AVP/UDP;unicast;client_port=" + String(session.clientRtpPort) + "-" + String(session.clientRtpPort + 1);
        transportResponse += ";server_port=" + String(session.serverRtpPort) + "-" + String(session.serverRtpPort + 1);
        transportResponse += ";source=" + WiFi.localIP().toString();
        transportResponse += ";ssrc=" + String(rtpSSRC, HEX);
    } else {
        session.transport = TRANSPORT_TCP;
        transportResponse = "RTP/AVP/TCP;unicast;interleaved=0-1";
    }
}

// RTSP handling
void handleRTSPCommand(ClientSession &session, String request, uint8_t clientIdx) {
    String response;
    response.reserve(512);
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }
    session.profileIndex = detectProfileFromRequest(request);
    String requestUserAgent = rtspHeaderValue(request, "User-Agent");
    if (requestUserAgent.length()) {
        requestUserAgent.replace("\r", "");
        requestUserAgent.replace("\n", "");
        requestUserAgent.toCharArray(session.userAgent, sizeof(session.userAgent));
    }

    // Reject disabled streams
    if (!streamEnabled[session.profileIndex]) {
        response += "RTSP/1.0 454 Session Not Found\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "\r\n";
        simplePrintln("Rejected disabled stream" + String(session.profileIndex + 1) + " from " + session.client.remoteIP().toString());
        queueRtspResponse(session, response);
        return;
    }

    int methodEnd = request.indexOf(' ');
    if (methodEnd <= 0) methodEnd = request.length();
    lastRtspCommand = request.substring(0, methodEnd);
    lastRtspCommandMs = millis();
    lastRTSPActivity = lastRtspCommandMs;

    if (request.startsWith("OPTIONS")) {
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n";

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic " + streamPath + " (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Content-Type: application/sdp\r\n";
        response += "Content-Base: rtsp://" + ip + ":8554" + streamPath + "/\r\n";
        response += "Content-Length: " + String(sdp.length()) + "\r\n\r\n";
        response += sdp;

    } else if (request.startsWith("SETUP")) {
        session.sessionId = String(random(100000000, 999999999));
        rtspSessionId = session.sessionId;
        String transportResponse;
        parseTransportHeader(session, request, transportResponse, clientIdx);
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n";
        response += "Transport: " + transportResponse + "\r\n\r\n";

    } else if (request.startsWith("PLAY")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n";
        response += "Range: npt=0.000-\r\n";
        response += "RTP-Info: url=rtsp://" + ip + ":8554" + streamPath + "/track1;seq=0;rtptime=0\r\n\r\n";

        bool wasAnyStreaming = anyRtspSessionStreaming();
        bool wasProfileStreaming = anyRtspSessionStreaming(session.profileIndex);
        if (!wasAnyStreaming) {
            flushAudioRingBuffer();
        }
        session.streaming = true;
        session.rtpSequence = 0;
        session.rtpTimestamp = 0;
        session.packetsSent = 0;
        session.playStartedAtMs = millis();
        session.lastRtpPacketMs = session.playStartedAtMs;
        if (!wasAnyStreaming) {
            audioPacketsSent = 0;
            lastStatsReset = millis();
            recentAudioRate.reset(lastStatsReset, audioPacketsSent);
            streamStartedAtMs = millis();
        }
        lastRtspPlayMs = millis();
        streamStats[session.profileIndex].streaming = true;
        streamStats[session.profileIndex].lastPlayMs = millis();
        if (!wasProfileStreaming) {
            streamStats[session.profileIndex].packetsSent = 0;
            streamStats[session.profileIndex].statsResetMs = millis();
            recentStreamRate[session.profileIndex].reset(
                streamStats[session.profileIndex].statsResetMs, 0);
        }
        rtspPlayCount++;
        lastRtpPacketMs = millis();
        simplePrintln("STREAMING STARTED stream" + String(session.profileIndex + 1));
        mqttPublishState(true);

    } else if (request.startsWith("TEARDOWN")) {
        unsigned long now = millis();
        IPAddress remoteIp = session.client.remoteIP();
        unsigned long sessionAgeMs = session.connectedAtMs ? now - session.connectedAtMs : 0;
        unsigned long streamAgeMs = session.playStartedAtMs ? now - session.playStartedAtMs : 0;
        unsigned long lastRtpAgeMs = session.lastRtpPacketMs ? now - session.lastRtpPacketMs : 0;
        String teardownLog;
        teardownLog.reserve(260);
        teardownLog += F("RTSP TEARDOWN: client=");
        teardownLog += remoteIp.toString();
        teardownLog += F(" slot=");
        teardownLog += String(clientIdx);
        teardownLog += F(" stream=");
        teardownLog += String(session.profileIndex + 1);
        teardownLog += F(" transport=");
        teardownLog += session.transport == TRANSPORT_UDP ? F("UDP") : F("TCP");
        teardownLog += F(" cseq=");
        teardownLog += cseq;
        teardownLog += F(" session_age_ms=");
        teardownLog += String(sessionAgeMs);
        teardownLog += F(" stream_age_ms=");
        teardownLog += String(streamAgeMs);
        teardownLog += F(" packets=");
        teardownLog += String(session.packetsSent);
        teardownLog += F(" last_rtp_ms=");
        teardownLog += String(lastRtpAgeMs);
        teardownLog += F(" rssi=");
        teardownLog += String(WiFi.RSSI());
        teardownLog += F("dBm user_agent=");
        teardownLog += session.userAgent[0] ? session.userAgent : "unknown";
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n\r\n";
        bool wasStreaming = session.streaming;
        session.streaming = false;
        bool anyStillStreaming = false;
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (i != clientIdx && clients[i].streaming && clients[i].profileIndex == session.profileIndex) {
                anyStillStreaming = true; break;
            }
        }
        if (!anyStillStreaming) streamStats[session.profileIndex].streaming = false;
        rtspTeardownCount++;
        if (wasStreaming) recordStreamStop("Client requested TEARDOWN", session.profileIndex + 1);
        simplePrintln(teardownLog);
        if (wasStreaming) simplePrintln("STREAMING STOPPED (" + lastStreamStopReason + ")");
        mqttPublishState(true);
    } else if (request.startsWith("GET_PARAMETER")) {
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n\r\n";
    } else {
        response += "RTSP/1.0 501 Not Implemented\r\n";
        response += "CSeq: " + cseq + "\r\n\r\n";
        simplePrintln("RTSP unsupported command: " + lastRtspCommand);
    }
    queueRtspResponse(session, response);
}

// RTSP processing
void processRTSP(ClientSession &session, uint8_t clientIdx) {
    if (!session.client.connected() || session.tcpWriter.pending()) return;
    // Bound input work too. TCP RTCP frames can contain NUL bytes or exceed
    // the text buffer; discard their declared binary length before parsing.
    uint8_t passes = 0;
    while (passes++ < 8) {
        if (session.interleavedBytesRemaining && session.parseBufferPos) {
            int skip = session.parseBufferPos;
            if (skip > session.interleavedBytesRemaining) skip = session.interleavedBytesRemaining;
            session.interleavedBytesRemaining -= skip;
            session.parseBufferPos -= skip;
            memmove(session.parseBuffer, session.parseBuffer + skip, session.parseBufferPos);
            session.parseBuffer[session.parseBufferPos] = '\0';
            continue;
        }
        if (!session.interleavedBytesRemaining && session.parseBufferPos >= 4 && session.parseBuffer[0] == '$') {
            session.interleavedBytesRemaining = (uint16_t(session.parseBuffer[2]) << 8) | session.parseBuffer[3];
            session.parseBufferPos -= 4;
            memmove(session.parseBuffer, session.parseBuffer + 4, session.parseBufferPos);
            session.parseBuffer[session.parseBufferPos] = '\0';
            continue;
        }
        char* endOfHeader = (!session.interleavedBytesRemaining && session.parseBuffer[0] != '$')
            ? strstr((char*)session.parseBuffer, "\r\n\r\n") : nullptr;
        if (endOfHeader) {
            int headerLen = (endOfHeader - (char*)session.parseBuffer) + 4;
            *endOfHeader = '\0';
            String request((char*)session.parseBuffer);
            int remaining = session.parseBufferPos - headerLen;
            if (remaining > 0) memmove(session.parseBuffer, session.parseBuffer + headerLen, remaining);
            session.parseBufferPos = remaining;
            session.parseBuffer[remaining] = '\0';
            handleRTSPCommand(session, request, clientIdx);
            if (!session.client.connected() || session.tcpWriter.pending()) return;
            continue;
        }
        int available = session.client.available();
        if (available <= 0) return;
        int room = sizeof(session.parseBuffer) - session.parseBufferPos - 1;
        if (room <= 0) {
            simplePrintln("RTSP buffer overflow - resetting");
            session.parseBufferPos = 0;
            session.parseBuffer[0] = '\0';
            return;
        }
        if (available > room) available = room;
        int bytesRead = session.client.read(session.parseBuffer + session.parseBufferPos, available);
        if (bytesRead <= 0) return;
        session.parseBufferPos += bytesRead;
        session.parseBuffer[session.parseBufferPos] = '\0';
    }
}

static int findFreeClientSlot() {
    for (uint8_t i = 0; i < maxActiveClients; i++) {
        if (!clients[i].client.connected()) return (int)i;
    }
    return -1;
}

void getStreamClientCounts(uint8_t &s1, uint8_t &s2) {
    s1 = 0; s2 = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) {
            if (clients[i].profileIndex == 1) s2++; else s1++;
        }
    }
}

uint8_t getRtspClientCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) count++;
    }
    return count;
}

String getRtspClientSummary() {
    String summary;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].client.connected()) continue;
        if (summary.length()) summary += ",";
        summary += clients[i].client.remoteIP().toString();
        summary += "/audio";
        summary += String(clients[i].profileIndex + 1);
    }
    return summary;
}

void stopRtspClientsForStream(uint8_t profileIndex, const char* reason) {
    if (profileIndex >= 2) return;
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].profileIndex != profileIndex) continue;
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    streamStats[profileIndex].streaming = false;
    streamStats[profileIndex].clientCount = 0;
    isStreaming = anyRtspSessionStreaming();
    if (!isStreaming) {
        rtspClient = WiFiClient();
        rtspSessionId = "";
    }
    if (hadStreaming && reason && reason[0]) {
        recordStreamStop(reason, profileIndex + 1);
    }
}

void stopAllRtspClients(const char* reason) {
    browserAudioStop();
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    for (uint8_t i = 0; i < 2; i++) {
        streamStats[i].streaming = false;
        streamStats[i].clientCount = 0;
    }
    rtspClient = WiFiClient();
    rtspSessionId = "";
    isStreaming = false;
    if (hadStreaming && reason && reason[0]) {
        recordStreamStop(reason, 0);
    }
}

static void refreshLegacyRtspState() {
    isStreaming = false;
    rtspClient = WiFiClient();
    rtspSessionId = "";
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected() && !rtspClient.connected()) {
            rtspClient = clients[i].client;
            lastRtspClientIp = rtspClient.remoteIP().toString();
        }
        if (clients[i].streaming) {
            isStreaming = true;
            if (rtspSessionId.length() == 0) rtspSessionId = clients[i].sessionId;
        }
    }
}


// Web UI is a separate module (WebUI.*)

// True minimum internal heap (the Web UI minimum is sampled and misses short
// Wi-Fi bursts) and PSRAM. Logged at boot; every 5 minutes with LOG_MEMORY_TREND.
void logMemorySnapshot(const char* label) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    simplePrintln(String(label) + " heap: free " + String((uint32_t)(heap_caps_get_free_size(caps) / 1024)) +
                  " KB, true min " + String((uint32_t)(heap_caps_get_minimum_free_size(caps) / 1024)) +
                  " KB, largest " + String((uint32_t)(heap_caps_get_largest_free_block(caps) / 1024)) +
                  " KB; PSRAM " + String((uint32_t)(ESP.getPsramSize() / 1024)) + " KB, free " +
                  String((uint32_t)(ESP.getFreePsram() / 1024)) + " KB");
}

void logHeapTrend() {
#if LOG_MEMORY_TREND
    static unsigned long lastLogMs = 0;
    if (millis() - lastLogMs < 300000UL) return;
    lastLogMs = millis();
    logMemorySnapshot("5-min");
    simplePrintln("5-min stream: drops " + String(audioRingBufferDropCount.load(std::memory_order_relaxed)) +
                  ", flushes " + String(audioRingBufferFlushCount) +
                  ", TCP stalls " + String(rtspWriteStallCount) +
                  ", write failures " + String(rtspWriteFailCount) +
                  ", ring " + String((uint32_t)audioRingBufferCapacityBytes) +
                  (audioRingInPsram ? " bytes (PSRAM)" : " bytes (internal)"));
#endif
}

void setup() {
    Serial.begin(115200);
    delay(100);

    // Complete an explicitly requested flash migration before audio, Wi-Fi,
    // temperature recovery or any scheduled actions can run.
    flashlayout::begin();

    // (4) seed for random(): combination of time and unique MAC
    randomSeed((uint32_t)micros() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF));

    bootTime = millis(); // Store boot time
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);
    mqttDeviceId = sanitizeMqttClientId(String("esp32mic_") + buildMqttMacSuffix(), "esp32mic");
    preloadTimeSettingsForEarlyLogs();
    loadBootMetadata();
    simplePrintln("Boot reason: " + rebootReason + ", restart #" + String(restartCounter));

    // Load and apply the saved RF path before Wi-Fi starts. Existing devices
    // keep external; changing the saved choice takes effect only after reboot.
    antenna::begin();
    if (antenna::supported()) {
        simplePrintln(antenna::external() ? "WiFi antenna: external (GPIO14 HIGH)"
                                         : "WiFi antenna: internal (GPIO14 LOW)");
    }
    logMemorySnapshot("Boot");

    // Load settings from flash
    loadAudioSettings();
    recording::begin();
    fieldpower::begin();

    // Allocate buffers with current size
    i2s_32bit_buffer = (int32_t*)audioWorkAlloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)audioWorkAlloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer || !i2s_16bit_network_buffer || !rtpPacketScratch) {
        simplePrintln("FATAL: Memory allocation failed!");
        ESP.restart();
    }

    // WiFi optimization for stable streaming
    WiFi.setSleep(false);

    wifiApLoad();
    devicesetup::begin(recording::mode());
    recording::setRuntimeMode(devicesetup::defaultRecording() && recordingHardwareSupported());
    webui_logModeChanged(recording::mode());
    devicesetup::currentMode(recording::mode());
    simplePrintln("WiFi: non-blocking connection; automatic fallback available.");

    // Apply configured WiFi TX power after connect (logs once on change)
    applyWifiTxPower(true);

    // Attempt initial NTP sync (non-blocking). Timezone/offset is configured in loadAudioSettings().
    if (timeSyncEnabled) {
        attemptTimeSync(false);
    }
    applyMdnsSetting();
    logDeepSleepWakeSnapshotIfAny();

    setupOTA();
    audioDmaWindow.reset(millis(), getI2sDmaOverrunTotal());
    if (!setup_i2s_driver()) {
        simplePrintln("FATAL: I2S setup failed!");
        ESP.restart();
    }
    if (!recording::mode() && !overheatLatched && !otaInProgress) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    } else {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    bool setupSchedTimeValid = false;
    bool setupSchedAllow = isStreamScheduleAllowedNow(&setupSchedTimeValid);
    if (streamScheduleEnabled && setupSchedTimeValid && !setupSchedAllow && rtspServerEnabled) {
        rtspServerEnabled = false;
        rtspServer.stop();
        simplePrintln("Startup: stream schedule outside window, RTSP server paused.");
    }
    // Web UI
    webui_begin();
    browserAudioBegin();
    simplePrintln("Listen queue: " + String((uint32_t)browserAudioQueueBytes()) + " bytes");
    mqttRequestReconnect(true);

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    float initialTemp = readChipTemperature();
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
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    setCpuFrequencyMhz(cpuFrequencyMhz);
    simplePrintln("CPU frequency set to " + String(cpuFrequencyMhz) + " MHz for optimal thermal/performance balance");

    if (!overheatLatched && rtspServerEnabled) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL1 (IP): rtsp://" + WiFi.localIP().toString() + ":8554/audio1");
        simplePrintln("RTSP URL2 (IP): rtsp://" + WiFi.localIP().toString() + ":8554/audio2");
        if (mdnsEnabled) {
            simplePrintln("RTSP URL1 (mDNS): rtsp://" + mdnsHostname + ".local:8554/audio1");
            simplePrintln("RTSP URL2 (mDNS): rtsp://" + mdnsHostname + ".local:8554/audio2");
        }
        simplePrintln("You can stream via IP or mDNS (if enabled).");
    } else if (overheatLatched) {
        simplePrintln("RTSP server paused due to thermal latch. Waiting for cooling/recovery policy.");
    } else if (recording::mode()) {
        simplePrintln("Recording mode: RTSP disabled, browser Listen available.");
    } else {
        simplePrintln("RTSP server paused by stream schedule. It will resume in the next allowed window.");
    }
    if (recording::mode()) {
        recordingAutoStartPending = recording::settings().autoStart && !overheatLatched;
        if (!recordingAutoStartPending) recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
    if (mqttEnabled) {
        simplePrintln("MQTT: enabled (" + mqttHost + ":" + String(mqttPort) +
                      "), topic " + mqttTopicPrefix +
                      ", discovery " + mqttDiscoveryPrefix +
                      ", interval " + String((uint32_t)mqttPublishIntervalSec) + "s");
    } else {
        simplePrintln("MQTT: disabled");
    }
}

// Explicit diagnostic actions may arrange one recovery reboot/timed sleep.
// Ordinary Wi-Fi-off and low-battery sleep never arm a wake timer.
struct FieldPowerTestReceipt {
    uint32_t magic, kind, radioMode, offlineKiB, elapsedSeconds, writtenKiB;
    uint32_t verifiedBytes, droppedSamples, dmaDelta, i2sDelta, saved;
};
RTC_NOINIT_ATTR static FieldPowerTestReceipt fieldPowerReceipt;
static uint32_t fieldTestAt = 0, fieldTestWake = 0, fieldTestBaseKiB = 0;
static uint32_t fieldTestOfflineWake = 0;
static uint32_t fieldTestDma = 0, fieldTestI2s = 0;
static bool fieldTestRadioSeen = false, fieldPowerStopRequested = false, fieldPowerSaveFinished = false;

void cancelRecordingAutoStart() { recordingAutoStartPending = false; }

String fieldPowerTestJson() {
    if (fieldPowerReceipt.magic != 0x46505431) return "null";
    const auto& r = fieldPowerReceipt;
    return String("{\"kind\":") + r.kind + ",\"radio_mode\":" + r.radioMode +
        ",\"offline_kib\":" + r.offlineKiB + ",\"elapsed_seconds\":" + r.elapsedSeconds +
        ",\"written_kib\":" + r.writtenKiB + ",\"verified_bytes\":" + r.verifiedBytes +
        ",\"dropped_samples\":" + r.droppedSamples + ",\"dma_delta\":" + r.dmaDelta +
        ",\"i2s_delta\":" + r.i2sDelta + ",\"saved\":" + (r.saved ? "true" : "false") +
        ",\"wake_cause\":" + uint32_t(esp_sleep_get_wakeup_cause()) + "}";
}

static void storeFieldPowerReceipt(uint32_t kind) {
    const auto r = recording::status();
    fieldPowerReceipt = {0, kind, uint32_t(WiFi.getMode()),
        fieldTestRadioSeen && r.writtenKiB >= fieldTestBaseKiB ? r.writtenKiB-fieldTestBaseKiB : 0,
        r.elapsedSeconds, r.writtenKiB, r.verifiedBytes, r.droppedSamples,
        getI2sDmaOverrunTotal()-fieldTestDma, audioI2SErrorCount-fieldTestI2s,
        uint32_t(r.state == recording::State::Saved && r.verified)};
    fieldPowerReceipt.magic = 0x46505431;
}

bool requestRecordingWifiOff(uint32_t testSeconds = 0) {
    if (!recording::mode() || (!testSeconds && recording::status().state != recording::State::Recording) ||
        (testSeconds && (testSeconds < 30 || testSeconds > 300)) || fieldTestAt || fieldTestWake || fieldTestOfflineWake) return false;
    const uint32_t delayMs = uint32_t(fieldpower::settings().wifiDelaySeconds)*1000;
    if (!devicesetup::requestWifiOff(delayMs)) return false;
    browserAudioStop();
    simplePrintln("Recording: Wi-Fi off requested; delay=" + String(fieldpower::settings().wifiDelaySeconds) + "s; device sleeps when recording finishes");
    if (testSeconds) {
        fieldPowerReceipt.magic = 0;
        fieldTestDma = getI2sDmaOverrunTotal(); fieldTestI2s = audioI2SErrorCount;
        fieldTestRadioSeen = false;
        fieldTestAt = millis() + max(uint32_t(250), delayMs) + testSeconds*1000;
    }
    return true;
}

bool requestBatterySleepTest(uint32_t wakeSeconds) {
    const auto r = battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant);
    const auto s = fieldpower::settings();
    if (wakeSeconds < 10 || wakeSeconds > 120 || fieldTestAt || fieldTestWake || fieldTestOfflineWake ||
        !recording::mode() || !s.guardEnabled ||
        !s.sleepAfterSave || !r.valid) return false;
    fieldPowerReceipt.magic = 0;
    fieldTestDma = getI2sDmaOverrunTotal(); fieldTestI2s = audioI2SErrorCount;
    fieldTestWake = wakeSeconds;
    return true;
}

// Explicit diagnostics only: exercise the normal offline end/sleep path, with
// a one-shot timer so a remote test can inspect the receipt and restore settings.
bool requestOfflineSleepTest(uint32_t wakeSeconds) {
    if (wakeSeconds < 10 || wakeSeconds > 120 || !recording::mode() ||
        fieldTestAt || fieldTestWake || fieldTestOfflineWake) return false;
    const auto state = recording::status().state;
    if (state != recording::State::Recording && state != recording::State::Saved && state != recording::State::Error) return false;
    if (!devicesetup::requestWifiOff(uint32_t(fieldpower::settings().wifiDelaySeconds)*1000)) return false;
    fieldPowerReceipt.magic = 0;
    fieldTestDma = getI2sDmaOverrunTotal(); fieldTestI2s = audioI2SErrorCount;
    fieldTestRadioSeen = false;
    fieldTestOfflineWake = wakeSeconds;
    browserAudioStop();
    return true;
}

// Only the application loop writes the Web UI's String log buffer.
void flushRecordingLogs() {
    String line;
    while (recording::nextLog(line)) {
        const String stamped = "[" + formatLogTimestamp() + "] " + line;
        Serial.println(stamped);
        webui_pushRecordingLog(stamped);
    }
}

static void sleepRecordingDevice(uint32_t receipt, const char* reason) {
    const uint32_t wake = fieldTestWake ? fieldTestWake : fieldTestOfflineWake;
    if (wake) storeFieldPowerReceipt(receipt);
    flushRecordingLogs();
    simplePrintln(reason);
    pauseAudioCapture();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (wake) esp_sleep_enable_timer_wakeup(uint64_t(wake)*1000000ULL);
    esp_deep_sleep_start();
}

static void serviceFieldPower() {
    static bool wifiErrorReported = false;
    if (*devicesetup::wifiOffError()) {
        if (!wifiErrorReported) simplePrintln("Recording: Wi-Fi switch-off failed; control connection restored, retry available");
        wifiErrorReported = true;
    } else wifiErrorReported = false;
    auto reading = battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant);
    // Explicit sleep diagnostic simulates low voltage without changing ADC calibration,
    // saved thresholds or the normal (untimed) shutdown behavior.
    if (fieldTestWake && reading.valid) {
        reading.millivolts = fieldpower::settings().cutoffMv - 100;
        reading.sequence = millis();
    }
    // Protection covers the entire Recording mode, including idle, completed
    // and error states. Stopping a session must not disable battery protection.
    const bool triggeredNow = fieldpower::observe(recording::mode(), reading);
    if (triggeredNow || !fieldpower::triggered()) fieldPowerStopRequested = fieldPowerSaveFinished = false;
    if (fieldpower::triggered() && !fieldPowerStopRequested) {
        recordingAutoStartPending = false;
        cancelThermalResume();
        recording::requestStop(recording::StopReason::LowBattery);
        fieldPowerStopRequested = true;
        simplePrintln("Low battery: stopping recording and closing the WAV file");
    }
    if (fieldpower::triggered() && !recording::busy() && !fieldPowerSaveFinished) {
        const auto state = recording::status().state;
        fieldpower::saved(state != recording::State::Error);
        fieldPowerSaveFinished = true;
        if (fieldpower::settings().sleepAfterSave && !fieldTestAt) {
            sleepRecordingDevice(2, "Recording: low-battery stop completed; entering deep sleep until reset/power cycle");
            return;
        }
    }
    // The worker clears busy only after WAV close, final event/journal sync and
    // unmount. Cooling/resume and OTA/reboot paths must finish their own work.
    const auto finalState = recording::status().state;
    if (recording::mode() && devicesetup::wifiSuspended() && !recording::busy() &&
        !recording::resumePending() && !recordingAutoStartPending && !scheduledRebootAt && !fieldTestAt &&
        (finalState == recording::State::Saved || finalState == recording::State::Error)) {
        sleepRecordingDevice(3, "Recording: offline session ended; entering deep sleep until reset/power cycle");
        return;
    }
    if (recordingAutoStartPending && !recording::busy() && !overheatLatched &&
        fieldpower::canStart(reading)) {
        recordingAutoStartPending = false;
        startRecording();
    }
    if (fieldTestAt || fieldTestOfflineWake) {
        if (devicesetup::wifiSuspended() && !fieldTestRadioSeen) {
            fieldTestRadioSeen = true; fieldTestBaseKiB = recording::status().writtenKiB;
        }
    }
    if (fieldTestAt) {
        if (int32_t(millis()-fieldTestAt) >= 0) {
            recording::requestStop();
            if (!recording::busy()) { storeFieldPowerReceipt(1); ESP.restart(); }
        }
    }
}

void loop() {
    flashlayout::loop();
    if (!flashlayout::busy()) {
        if (!recording::busy() && !devicesetup::wifiSuspended()) ArduinoOTA.handle();
        wifiApRecoveryLoop();
    }

    if (!devicesetup::wifiSuspended() && millis() - lastWebuiHandleMs >= WEBUI_HANDLE_INTERVAL_MS) {
        webui_handleClient();
        lastWebuiHandleMs = millis();
    }

    // Pause temperature checks, thermal recovery, schedules/deep sleep, MQTT
    // and audio health actions throughout preparation and its deferred reboot.
    // Saved thermal protection and any existing shutdown latch stay untouched.
    if (otaInProgress || flashlayout::busy()) {
        if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt && recording::stop()) ESP.restart();
        delay(1);
        return;
    }

    battery::loop(devicesetup::batteryMonitoring(), s3BoardVariant);
    flushRecordingLogs();
    serviceFieldPower();

    if (millis() - lastTempCheck > TEMPERATURE_CHECK_INTERVAL_MS) {
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

    if (millis() - lastProducerHealthCheck >= 1000UL) {
        updateAudioDiagnostics();
        logHeapTrend();
        checkMicrophoneSignal();
        checkAudioProducerHealth();
        lastProducerHealthCheck = millis();
    }

    if (millis() - lastWiFiCheck > 30000) { // 30 s
        checkWiFiHealth(); // without TX power log spam
        lastWiFiCheck = millis();
    }

    checkTimeSync();
    if (millis() - lastStreamScheduleCheck > 1000UL) {
        checkStreamSchedule();
        checkDeepSleepSchedule();
        lastStreamScheduleCheck = millis();
    }

    checkScheduledReset();

    // RTSP client management (configurable 1-3 concurrent sessions)
    if (rtspServerEnabled) {
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            // WiFiClient::operator bool() calls connected(); do not combine it
            // with !connected(), which would miss established disconnections.
            if ((clients[i].connectedAtMs != 0 || clients[i].streaming) && !clients[i].client.connected()) {
                bool wasStreaming = clients[i].streaming;
                if (wasStreaming) {
                    rtspDisconnectCount++;
                    recordStreamStop("TCP client disconnected", clients[i].profileIndex + 1);
                }
                clients[i].reset();
                if (wasStreaming) mqttPublishState(true);
            }
            if (clients[i].client.connected() && !clients[i].streaming) {
                if (millis() - clients[i].lastActivity > 30000UL) {
                    clients[i].reset();
                }
            }
        }

        WiFiClient newClient = rtspServer.accept();
        if (newClient) {
            int slot = findFreeClientSlot();
            if (slot >= 0) {
                clients[slot].client = newClient;
                clients[slot].client.setNoDelay(true);
                clients[slot].lastActivity = millis();
                clients[slot].connectedAtMs = clients[slot].lastActivity;
                clients[slot].playStartedAtMs = 0;
                clients[slot].lastRtpPacketMs = 0;
                clients[slot].userAgent[0] = '\0';
                clients[slot].parseBufferPos = 0;
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                simplePrintln("New RTSP client connected in slot " + String(slot) + " from: " + newClient.remoteIP().toString());
                mqttPublishState(true);
            } else {
                newClient.stop();
                simplePrintln("RTSP client rejected: max sessions reached");
            }
        }

        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].client.connected()) {
                servicePendingTcp(clients[i]);
                if (!clients[i].client.connected()) continue;
                drainRtcpPackets(clients[i]);
                if (clients[i].client.available()) {
                    clients[i].lastActivity = millis();
                    lastRTSPActivity = clients[i].lastActivity;
                }
                processRTSP(clients[i], i);
            }
        }
        streamAudio();
    } else if (!recording::mode()) {
        // Mode entry already disconnected RTSP. Do not repeatedly cancel the
        // independent browser Listen session while Recording keeps RTSP off.
        stopAllRtspClients("RTSP server disabled");
    }
    refreshLegacyRtspState();
    browserAudioLoop((rtspServerEnabled || recording::mode()) && !overheatLatched && audioProducerRunning &&
                     !audioProducerStopRequested && (WiFi.status() == WL_CONNECTED || wifiApRecoveryActive()), currentSampleRate);
    uint32_t rateNow = millis();
    recentAudioRate.update(rateNow, audioPacketsSent);
    for (uint8_t i = 0; i < 2; ++i) recentStreamRate[i].update(rateNow, streamStats[i].packetsSent);
    checkMqtt();
    // Handle deferred WiFi reconnect
    if (wifiReconnectAt != 0 && !devicesetup::wifiSuspended() && !devicesetup::wifiOffPending() && millis() >= wifiReconnectAt) {
        wifiReconnectAt = 0;

        bool wasStreaming = isStreaming;
        stopAllRtspClients("WiFi reconnect requested");
        refreshLegacyRtspState();
        if (wasStreaming) mqttPublishState(true);

        devicesetup::reconnect(wifiReconnectHasBssid ? wifiReconnectBssid : nullptr);
        wifiReconnectHasBssid = false;
    }

    // Handle deferred reboot/reset safely here
    if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt && recording::stop()) {
        if (scheduledFactoryReset) {
            resetToDefaultSettings();
        }
        delay(50);
        ESP.restart();
    }
}
