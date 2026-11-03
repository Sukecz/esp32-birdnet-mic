#ifndef BIRDNET_APP_RUNTIME_H
#define BIRDNET_APP_RUNTIME_H
// Transitional application bridge. Modules own their private transport/lifecycle
// state; this is the canonical declaration of the existing shared runtime view.
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include "freertos/ringbuf.h"
#include <Preferences.h>
#include "AudioSettings.h"
#include "StreamHealth.h"
#include "StreamTypes.h"
#include "AudioDiagnostics.h"
#include "MicrophoneSignal.h"
#include "MicrophoneProfiles.h"
#include "S3Boards.h"
#include "ThermalRecovery.h"
#include "Recording.h"
#include "DeviceSetup.h"
#include "FlashLayout.h"
#include "BrowserAudio.h"
#include "WebUI.h"
extern WiFiServer rtspServer;
extern WiFiClient rtspClient;
extern volatile bool isStreaming;
extern uint16_t rtpSequence;
extern uint32_t rtpTimestamp;
extern unsigned long lastStatsReset;
extern unsigned long lastRtspPlayMs;
extern uint32_t rtspPlayCount;
extern unsigned long lastRtspClientConnectMs;
extern unsigned long bootTime;
extern unsigned long lastRTSPActivity;
extern unsigned long lastWiFiCheck;
extern unsigned long lastTempCheck;
extern uint32_t minFreeHeap;
extern float maxTemperature;
extern bool rtspServerEnabled;
extern uint32_t audioPacketsSent;
extern uint32_t audioI2SErrorCount;
extern std::atomic<uint32_t> audioRingBufferDropCount;
extern uint32_t audioRingBufferChunkCount;
extern uint32_t audioRingBufferFlushCount;
extern uint32_t rtspWriteStallCount;
extern uint32_t rtspWriteTimeoutCount;
extern size_t audioRingBufferCapacityBytes;
extern bool audioRingInPsram;
extern volatile bool audioProducerRunning;
extern volatile bool audioProducerStopRequested;
extern volatile uint32_t audioProducerLastReadMs;
extern std::atomic<uint32_t> audioProcessingLoadSnapshot;
extern DmaOverrunStatus getI2sDmaStatus();
extern uint32_t currentSampleRate;
extern float currentGainFactor;
extern uint16_t currentBufferSize;
extern uint8_t i2sShiftBits;
extern uint8_t micFormat;
extern bool pdmMicSupported();
extern const char* pdmDecoderName();
extern uint8_t pdmRawWordStride();
extern const char* pdmRawBitOrder();
extern uint32_t minAcceptableRate;
extern uint32_t performanceCheckInterval;
extern bool autoRecoveryEnabled;
extern uint16_t cpuFrequencyMhz;
extern wifi_power_t currentWifiPowerLevel;
extern void resetToDefaultSettings();
extern bool autoThresholdEnabled;
extern uint32_t computeRecommendedMinRate();
extern bool scheduledResetEnabled;
extern uint32_t resetIntervalHours;
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);
extern bool requestOfflineSleepTest(uint32_t wakeSeconds);
extern volatile unsigned long wifiReconnectAt;
extern void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs);
extern MicrophoneSignal microphoneSignal;
extern uint16_t lastPeakAbs16;
extern uint32_t audioClipCount;
extern bool audioClippedLastBlock;
extern uint16_t peakHoldAbs16;
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool overheatLockoutActive;
extern uint8_t overheatConsecutiveSamples;
extern float overheatTripTemp;
extern unsigned long overheatTriggeredAt;
extern String overheatLastReason;
extern String overheatLastTimestamp;
extern bool overheatSensorFault;
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern bool overheatLatched;
extern bool overheatAutoResumeEnabled;
extern ThermalRecovery thermalRecovery;
extern bool acknowledgeThermalShutdown();
extern void cancelThermalResume();
extern void beginOtaUpdate();
extern void endOtaUpdate(bool installed);
extern StreamProfileConfig streamProfiles[2];
extern bool streamEnabled[2];
extern uint8_t maxActiveClients;
extern StreamStats streamStats[2];
extern uint32_t getCurrentPacketRate(uint8_t profileIndex);
extern void getStreamClientCounts(uint8_t &s1, uint8_t &s2);
extern String getRtspClientSummary();
extern void stopAllRtspClients(const char* reason);
extern float wifiPowerLevelToDbm(wifi_power_t lvl);
extern String formatUptime(unsigned long seconds);
extern String formatSince(unsigned long eventMs);
extern bool restartI2S();
extern bool recordingHardwareSupported();
extern bool recordingModeSupported();
extern bool startRecording();
extern void cancelRecordingAutoStart();
extern bool requestRecordingWifiOff(uint32_t testSeconds);
extern bool requestBatterySleepTest(uint32_t wakeSeconds);
extern String fieldPowerTestJson();
extern bool setDeviceMode(bool recordingMode);
extern bool applyAudioConfig(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift);
extern bool applyMicFormatConfig(uint8_t newFormat);
extern bool applyMicProfileConfig(uint8_t newFormat, bool recommended);
extern uint8_t s3BoardVariant;
extern bool s3BoardSelectionSupported();
extern bool builtinMicSelected();
extern bool applyS3BoardConfig(uint8_t variant);
extern s3boards::Config getS3BoardConfig();
extern bool restoreS3BoardConfig(const s3boards::Config& saved);
extern bool getRecommendedMicSettings(uint8_t format, micprofiles::Settings& out);
extern bool cpuFrequencySupported(uint32_t mhz);
extern uint16_t maxSupportedCpuFrequencyMhz();
extern uint16_t maxHighpassCutoffForRate(uint32_t sampleRate);
extern void saveAudioSettings();
extern void applyWifiTxPower(bool log);
extern const char* FW_VERSION_STR;
extern const char* FW_BOARD_ID_STR;
extern const char* FW_BOARD_NAME_STR;
extern const char* FW_CHIP_FAMILY_STR;
extern const char* FW_OTA_ARTIFACT_STR;
extern bool timeSynced;
extern unsigned long lastTimeSyncSuccess;
extern int32_t timeOffsetMinutes;
extern uint8_t timeZoneMode;
extern String customTimeZoneRule;
extern bool isValidCustomTimeZoneRule(const String& rule);
extern int32_t effectiveTimeOffsetMinutes();
extern bool timeSyncEnabled;
extern const char* timeSyncStatusCode();
extern bool mdnsEnabled;
extern bool mdnsRunning;
extern bool streamScheduleEnabled;
extern uint16_t streamScheduleStartMin;
extern uint16_t streamScheduleStopMin;
extern bool deepSleepScheduleEnabled;
extern String deepSleepStatusCode;
extern uint32_t deepSleepNextSleepSec;
extern bool mqttEnabled;
extern String mqttHost;
extern uint16_t mqttPort;
extern String mqttUser;
extern String mqttPassword;
extern String mqttTopicPrefix;
extern String mqttDiscoveryPrefix;
extern String mqttClientId;
extern uint16_t mqttPublishIntervalSec;
extern bool mqttConnected;
extern String mqttLastError;
extern bool mqttDiscoveryPublished;
extern uint32_t rtspWriteFailCount;
extern uint32_t rtspTeardownCount;
extern uint32_t rtspDisconnectCount;
extern String lastStreamStopReason;
extern String lastStreamStopTime;
extern uint8_t lastStreamStopStream;
extern unsigned long lastStreamStopMs;
extern bool isStreamScheduleAllowedNow(bool* timeValidOut = nullptr);
extern String mdnsHostname;
extern bool attemptTimeSync(bool logResult, bool quickMode);
extern String formatDateTime();
extern void configureTimeService(bool enableNtp);
extern void applyMdnsSetting();
extern void mqttRequestReconnect(bool forceDiscovery);
extern void mqttPublishDiscoverySoon();
extern unsigned long streamStartedAtMs;
extern uint32_t wifiReconnectCount;
extern uint32_t restartCounter;
extern String rebootReason;
extern const char* FW_BUILD_DATE_STR;
extern uint8_t getRtspClientCount();
extern void stopRtspClientsForStream(uint8_t, const char*);
extern void simplePrintln(String);
extern void stopAudioProducer();
extern bool startAudioProducer();
extern String buildMqttMacSuffix();
extern bool applicationUpdateInProgress();
extern Preferences audioPrefs;
extern bool highpassEnabled;
extern uint16_t highpassCutoffHz;
extern float wifiTxPowerDbm;
extern bool lastScheduleAllow;
extern bool lastScheduleTimeValid;
extern wifi_power_t pickWifiPowerLevel(float);
extern String defaultMdnsHostname();
extern String sanitizeMdnsHostname(const String&, const String&);
extern void updateS3BoardIdentity();
extern bool micBoardFormatAllowed(uint8_t);
extern const char* micFormatName(uint8_t);
extern RingbufHandle_t audioRingBuffer;
extern int16_t* i2s_16bit_network_buffer;
extern uint8_t* rtpPacketScratch;
extern bool audioRingBufferFlushPending;
extern void flushAudioRingBuffer();
extern unsigned long lastRtpPacketMs;
extern unsigned long lastRtspCommandMs;
extern String lastRtspCommand;
extern String lastRtspClientIp;
extern String rtspSessionId;
extern uint32_t rtpSSRC;
extern uint32_t rtspConnectCount;
extern void recordStreamStop(const char*, uint8_t);
extern RecentPacketRate recentAudioRate;
extern RecentPacketRate recentStreamRate[2];
extern bool otaInProgress;
extern unsigned long lastI2SReset;
extern uint32_t maxPacketRate;
extern uint32_t minPacketRate;
#endif
