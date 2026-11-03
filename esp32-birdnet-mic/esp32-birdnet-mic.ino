#include "AudioCapture.h"
#include "NoiseShadow.h"
#include "RtspService.h"
#include "FirmwareDefaults.h"
#include "SettingsStore.h"
#include "AppRuntime.h"
#include "MqttService.h"
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
#include "RecordingMemory.h"
#include "RecordingPins.h"
static uint32_t recordingDmaBaseline = 0;
static bool recordingAutoStartPending = false;
#include "ThermalRecovery.h"
#include "ChipTemperature.h"
#include "FlashLayout.h"
#include "AudioReserve.h"
#include <esp_memory_utils.h>

#include "AudioMemory.h"
#ifndef LOG_MEMORY_TREND
#define LOG_MEMORY_TREND 0  // 1: log heap, PSRAM and stream counters every 5 minutes
#endif
#define FXAUDIO_HOT IRAM_ATTR __attribute__((noinline))
#include "FixedAudio.h"

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi) ==================
#define FW_VERSION "2.06"
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
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
#define XIAO_BOARD_ID "esp32-s31-function-coreboard-1"
#define XIAO_BOARD_NAME "ESP32-S31-Function-CoreBoard-1"
#define XIAO_CHIP_FAMILY "ESP32-S31"
#define XIAO_OTA_ARTIFACT "firmware-app-s31.bin"
#define XIAO_HAS_RF_SWITCH 0
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

static const uint8_t OVERHEAT_CONFIRM_SAMPLES = 3;
static const float OVERHEAT_EMERGENCY_MARGIN_C = 10.0f;
static const unsigned long TEMPERATURE_CHECK_INTERVAL_MS = 5000UL;

// -- Pins


// -- Servers





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
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    return true;
#endif
    return s3BoardSelectionSupported() && s3boards::builtin(s3BoardVariant) && micFormat == 2;
}
bool micBoardFormatAllowed(uint8_t format) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    return format == 0;
#endif
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
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
    constexpr auto board = micprofiles::S31;
#else
    constexpr auto board = micprofiles::Unknown;
#endif
    if (!micBoardFormatAllowed(format) || !micprofiles::recommended(board, format, out)) return false;
    // Start conservatively with the built-in microphones; tune from the level meter.
    if (s3BoardSelectionSupported() && s3boards::builtin(s3BoardVariant) && format == 2) out.gain = DEFAULT_GAIN_FACTOR;
    return true;
}

uint16_t maxSupportedCpuFrequencyMhz() {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    return 320;
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C5)
    return 240;
#else
    return 160;
#endif
}

bool cpuFrequencySupported(uint32_t mhz) {
    return mhz == 40 || mhz == 80 || mhz == 160 ||
        (mhz == 240 && maxSupportedCpuFrequencyMhz() >= 240) ||
        (mhz == 320 && maxSupportedCpuFrequencyMhz() >= 320);
}

const char* micFormatName(uint8_t format) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    if (format == 0) return "ES8311/Philips";
#endif
    if (format == MIC_FORMAT_PDM) return "PDM";
    return format == MIC_FORMAT_MSB ? "SPH0645/MSB" : "ICS43434/Philips";
}












StreamProfileConfig streamProfiles[2] = {
    {STREAM_TARGET_BIRDNET_GO},
    {STREAM_TARGET_BIRDNET_PI},
};
bool streamEnabled[2] = {true, false};
uint8_t maxActiveClients = 2;


StreamStats streamStats[2];
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

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
uint8_t i2sShiftBits = 12;  // (1) compile-time default respected on first boot

// -- Preferences for persistent settings


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
uint16_t cpuFrequencyMhz = 160;          // CPU frequency (default 160 MHz)

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
// Shared between capture, RTSP recovery and Web UI; no other state is published
// through this counter, so relaxed ordering is sufficient.
std::atomic<uint32_t> audioRingBufferDropCount{0};
uint32_t audioRingBufferChunkCount = 0;
uint32_t audioRingBufferFlushCount = 0;
bool audioRingBufferFlushPending = false;
uint32_t rtspWriteStallCount = 0;
uint32_t rtspWriteTimeoutCount = 0;

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
wifi_power_t pickWifiPowerLevel(float dbm) {
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

String buildMqttMacSuffix() {
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

String defaultMdnsHostname() {
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

uint32_t getCurrentPacketRate(uint8_t profileIndex) {
    uint32_t now = millis();
    if (profileIndex < 2) {
        return streamStats[profileIndex].streaming
            ? recentStreamRate[profileIndex].rate(now, streamStats[profileIndex].packetsSent) : 0;
    }
    return isStreaming ? recentAudioRate.rate(now, audioPacketsSent) : 0;
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass)


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
bool isStreamScheduleAllowedNow(bool* timeValidOut) {
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
    mqttDisconnectForSleep();
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
    noiseshadow::resetSettings();
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
    const uint8_t oldFormat = micFormat;
    const uint16_t oldCpu = cpuFrequencyMhz;
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
    const uint16_t nextCpu = next.cpu ? next.cpu : oldCpu;
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


bool recordingModeSupported() {
    return recordingpins::modeSupported(s3BoardVariant);
}

bool recordingHardwareSupported() {
    return recordingpins::resolve(s3BoardVariant).supported;
}

bool startRecording() {
    if (recording::busy()) return false;
    if (!fieldpower::canStart(battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant)))
        return recording::rejectStart("Battery protection is blocking recording; check Battery");
    if (otaInProgress || flashlayout::busy())
        return recording::rejectStart("An update is in progress; wait before starting recording");
    if (overheatLatched)
        return recording::rejectStart("Thermal protection is blocking recording; check Thermal management");
    if (!audioProducerRunning || audioProducerStopRequested)
        return recording::rejectStart("Microphone capture is stopped; use Reset I2S before recording");
    if (scheduledRebootAt)
        return recording::rejectStart("A restart is pending; wait before starting recording");
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

bool applicationUpdateInProgress() { return otaInProgress; }

// Restart I2S with new parameters


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
    ArduinoOTA.setMdnsEnabled(mdnsEnabled);
    ArduinoOTA.begin();
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
    mqttInitializeIdentity();
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
    noiseshadow::begin();
    recording::begin();
    fieldpower::begin();

    if (!allocateAudioBuffers()) {
        simplePrintln("FATAL: Memory allocation failed!");
        ESP.restart();
    }

    // WiFi optimization for stable streaming
    WiFi.setSleep(false);

    wifiApLoad();
    devicesetup::begin(recording::mode());
    recording::setRuntimeMode(devicesetup::defaultRecording() && recordingpins::modeSupported(s3BoardVariant));
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
    resetAudioDmaWindow();
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
        serviceRtspClients();
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
