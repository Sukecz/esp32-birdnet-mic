#include "AppRuntime.h"
#include "SettingsStore.h"
#include "FirmwareDefaults.h"
#include "MqttService.h"
#include "RecordingPins.h"
#include "TimeZoneSchedule.h"
#include <esp_timer.h>
#include <math.h>

Preferences audioPrefs;
// Load settings from flash
void loadAudioSettings() {
    bool settingsRepaired = false;
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // (1) respect compile-time default 12 on first boot
    i2sShiftBits = audioPrefs.getUChar("shiftBits", i2sShiftBits);
    microphoneGainConfigured = audioPrefs.isKey("gainFactor");
    micAutoDetect = audioPrefs.isKey("micAuto") ? audioPrefs.getBool("micAuto", false) : !audioPrefs.isKey("micFormat");
    s3BoardAutoDetect = audioPrefs.isKey("boardAuto") ? audioPrefs.getBool("boardAuto", false) : !audioPrefs.isKey("s3Board");
    micFormat = audioPrefs.getUChar("micFormat", DEFAULT_MIC_FORMAT);
    recordingpins::senseSdPresent = audioPrefs.getBool("senseSd", false);
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
    audioPrefs.putBool("micAuto", micAutoDetect);
    audioPrefs.putBool("boardAuto", s3BoardAutoDetect);
    audioPrefs.putBool("senseSd", recordingpins::senseSdPresent);
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
