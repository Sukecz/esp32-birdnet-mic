#include "WebRuntime.h"
#include "OtaService.h"
#include "SettingsValidation.h"
#include "AppRuntime.h"
#include "Recording.h"
#include "DeviceLog.h"
#include "LogStorage.h"
#include "Antenna.h"
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <atomic>
#include <stdlib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include "DeviceSetup.h"
#include "SetupConfig.h"
#include "Battery.h"
#include "FieldPower.h"
#include "S3Boards.h"
#include "RecordingPins.h"
#include <ESPmDNS.h>
#include "WebUI.h"
#include "NoiseShadow.h"
#include "AudioDiagnostics.h"
#include "MicrophoneSignal.h"
#include "BrowserAudio.h"
#include <esp_heap_caps.h>
#include "MicrophoneProfiles.h"
#include "WifiAp.h"
#include "ThermalRecovery.h"
#include <algorithm>
#include <vector>
#include "WebUI_gz.h"
#include "SetupAssets_gz.h"
#include "OtaAssets_gz.h"
#include "FlashLayout.h"

// External variables and functions from main (.ino) – ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi

// Local helper: snap requested Wi‑Fi TX power (dBm) to nearest supported step
static float snapWifiTxDbm(float dbm) {
    static const float steps[] = {-1.0f, 2.0f, 5.0f, 7.0f, 8.5f, 11.0f, 13.0f, 15.0f, 17.0f, 18.5f, 19.0f, 19.5f};
    float best = steps[0];
    float bestd = fabsf(dbm - steps[0]);
    for (size_t i=1;i<sizeof(steps)/sizeof(steps[0]);++i){
        float d = fabsf(dbm - steps[i]);
        if (d < bestd){ bestd = d; best = steps[i]; }
    }
    return best;
}

static const uint32_t OH_MIN = 30;
static const uint32_t OH_MAX = 95;
static const uint32_t OH_STEP = 5;
static const char* UI_MUTATION_HEADER = "X-ESP32MIC-CSRF";
static const char* UI_MUTATION_TOKEN = "1";
// Web server and in-memory log ring buffer
WebServer web(80);
static DeviceLog logBuffer;
bool webui_logModeChanged(bool recordingMode) {
    const bool ok=logBuffer.configure(recordingMode);
    return ok || !recordingMode || logBuffer.usesPsram() || logBuffer.capacity()<=DeviceLog::kRecordingBytes;
}
static void pushLog(const String& line, bool persist) {
    logBuffer.configure(recording::mode());
    logBuffer.append(line.c_str(),line.length());
    if(persist)recording::queueSystemLog(line.c_str());
}

void webui_pushLog(const String& line) { pushLog(line, true); }
void webui_pushRecordingLog(const String& line) { pushLog(line, false); }

String jsonEscape(const String &s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i]; if(c=='"'||c=='\\'){o+='\\';o+=c;} else if((uint8_t)c<32){char escaped[7];snprintf(escaped,sizeof(escaped),"\\u%04x",(unsigned)(uint8_t)c);o+=escaped;} else {o+=c;}}
    return o;
}

static String formatLocalDateTimeSafe() {
    time_t now = time(nullptr);
    if (now <= 1672531200) return F("unavailable");
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) return F("unavailable");
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    return String(buf);
}

static String formatUtcDateTimeSafe() {
    time_t now = time(nullptr);
    if (now <= 1672531200) return F("unavailable");
    struct tm tmUtc;
    if (!gmtime_r(&now, &tmUtc)) return F("unavailable");
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmUtc);
    return String(buf);
}

static String profileName(uint16_t buf) {
    // Server-side fallback (English). UI localizes on client by buffer size.
    if (buf <= 256) return F("Ultra-Low Latency (Higher CPU, May have dropouts)");
    if (buf <= 512) return F("Balanced (Moderate CPU, Good stability)");
    if (buf <= 1024) return F("Stable Streaming (Lower CPU, Excellent stability)");
    return F("High Stability (Lowest CPU, Maximum stability)");
}

void apiSendJSON(const String &json) {
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "application/json", json);
}

bool hasMutationAuth() {
    if (web.hasHeader(UI_MUTATION_HEADER)) {
        String token = web.header(UI_MUTATION_HEADER);
        token.trim();
        if (token == UI_MUTATION_TOKEN) {
            return true;
        }
    }
    return false;
}

bool requireMutationAuth(bool preparation = false) {
    if (hasMutationAuth()) {
        if (recording::busy() && web.uri() != "/api/recording/action" && web.uri() != "/api/device/mode" &&
            web.uri() != "/api/setup/access") {
            web.send(409, "application/json", "{\"ok\":false,\"error\":\"Stop and save recording before changing settings or updating firmware.\"}");
            return false;
        }
        if (!preparation && flashlayout::busy()) {
            web.send(409, "application/json", "{\"ok\":false,\"error\":\"Update preparation is in progress. Please wait.\"}");
            return false;
        }
        return true;
    }

    web.sendHeader("Cache-Control", "no-cache");
    web.send(403, "application/json", "{\"ok\":false,\"error\":\"forbidden\"}");
    return false;
}

// Static browser assets are decompressed by the browser, never by the ESP32.
static void sendGzipAsset(const char* type, const uint8_t* bytes, size_t length) {
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("Content-Encoding", "gzip");
    web.sendHeader("Vary", "Accept-Encoding");
    web.send_P(200, type, reinterpret_cast<PGM_P>(bytes), length);
}

static void httpIndex() {
    if (!devicesetup::complete()) {
        sendGzipAsset("text/html; charset=utf-8", SETUP_HTML_GZ, SETUP_HTML_GZ_LEN);
        return;
    }
    sendGzipAsset("text/html; charset=utf-8", WEBUI_INDEX_GZ, WEBUI_INDEX_GZ_LEN);
}
static void httpSetupPage() { sendGzipAsset("text/html; charset=utf-8", SETUP_HTML_GZ, SETUP_HTML_GZ_LEN); }
static void httpSetupCss() { sendGzipAsset("text/css; charset=utf-8", SETUP_CSS_GZ, SETUP_CSS_GZ_LEN); }
static void httpSetupJs() { sendGzipAsset("application/javascript; charset=utf-8", SETUP_JS_GZ, SETUP_JS_GZ_LEN); }
static void httpApPasswordJs() { sendGzipAsset("application/javascript; charset=utf-8", AP_PASSWORD_JS_GZ, AP_PASSWORD_JS_GZ_LEN); }
static void httpOtaCss() {
    sendGzipAsset("text/css; charset=utf-8", OTA_CSS_GZ, OTA_CSS_GZ_LEN);
}
static void httpOtaJs() {
    sendGzipAsset("application/javascript; charset=utf-8", OTA_JS_GZ, OTA_JS_GZ_LEN);
}

// HTTP handlery

// OTA service implementation is owned by OtaService.cpp.

static String setupStatusJson() {
    String json = "{\"ok\":true,\"complete\":" + String(devicesetup::complete() ? "true" : "false");
    json += ",\"default_mode\":\"" + String(devicesetup::defaultRecording() ? "recording" : "streaming") + "\"";
    json += ",\"connection\":\"" + String(devicesetup::standalone() ? "direct" : "home") + "\"";
    json += ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += ",\"ssid\":\"" + jsonEscape(devicesetup::savedSsid()) + "\"";
    json += ",\"home_ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String()) + "\"";
    json += ",\"ap_name\":\"" + jsonEscape(devicesetup::apName()) + "\"";
    json += ",\"ap_active\":" + String(devicesetup::apActive() ? "true" : "false");
    json += ",\"ap_ip\":\"192.168.4.1\",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"test_state\":\"" + String(devicesetup::connectionState()) + "\"";
    json += ",\"test_error\":\"" + String(devicesetup::connectionError()) + "\"";
    json += ",\"chip_family\":\"" + jsonEscape(FW_CHIP_FAMILY_STR) + "\"";
    json += ",\"board_name\":\"" + jsonEscape(FW_BOARD_NAME_STR) + "\"";
    json += ",\"s3_board_supported\":" + String(s3BoardSelectionSupported() ? "true" : "false");
    json += ",\"s3_board\":" + String(s3BoardVariant);
    json += ",\"mic_format\":" + String(micFormat);
    json += ",\"battery_enabled\":" + String(devicesetup::batteryMonitoring() ? "true" : "false");
    json += ",\"battery_supported\":" + String(battery::pins(s3BoardVariant).supported ? "true" : "false");
    json += ",\"antenna_supported\":" + String(antenna::supported() ? "true" : "false");
    json += ",\"antenna\":\"" + String(antenna::external() ? "external" : "internal") + "\"}";
    return json;
}
static void httpSetupStatus() { apiSendJSON(setupStatusJson()); }
static void httpSetupAccess() {
    if (!requireMutationAuth()) return;
    if (web.hasArg("password")) {
        const String password = web.arg("password");
        if (web.hasArg("shorten") || !devicesetup::validApPassword(password.c_str(), password.length())) {
            web.send(400, "application/json", "{\"ok\":false,\"error\":\"Use 8\u201363 characters: letters without accents, numbers, spaces or symbols.\"}"); return;
        }
        if (recording::busy() || recording::resumePending() || !devicesetup::canChangeApPassword()) {
            web.send(409, "application/json", "{\"ok\":false,\"error\":\"Stop recording and finish Wi-Fi connection tests before changing the password.\"}"); return;
        }
        if (!devicesetup::setApPassword(password)) {
            web.send(503, "application/json", "{\"ok\":false,\"error\":\"Could not save the password. Try again.\"}"); return;
        }
    }
    if (web.hasArg("shorten")) {
        if (web.arg("shorten") != "1") {
            web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid password action.\"}"); return;
        }
        if (recording::busy() || recording::resumePending() || !devicesetup::shortenApPassword()) {
            web.send(409, "application/json", "{\"ok\":false,\"error\":\"Stop recording and disconnect device Wi-Fi clients before changing its password.\"}"); return;
        }
    }
    apiSendJSON("{\"ok\":true,\"password\":\"" + jsonEscape(devicesetup::apPassword()) + "\"}");
}

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    String localTimeStr = formatLocalDateTimeSafe();
    String utcTimeStr = formatUtcDateTimeSafe();
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
    uint32_t currentRate = getCurrentPacketRate(255);
    const String controlIp = (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
    String json = "{";
    json.reserve(1800);
    json += "\"wifi_antenna_supported\":" + String(antenna::supported() ? "true" : "false") + ",";
    json += "\"wifi_antenna\":" + (antenna::supported() ? String(antenna::external() ? "\"external\"" : "\"internal\"") : String("null")) + ",";
    json += "\"device_log\":{\"capacity_bytes\":" + String(logBuffer.capacity()) + ",\"used_bytes\":" + String(logBuffer.size()) + ",\"psram\":" + String(logBuffer.usesPsram()?"true":"false") + "},";
    json += "\"setup\":" + setupStatusJson() + ",";
    json += "\"battery\":" + battery::statusJson(devicesetup::batteryMonitoring(), s3BoardVariant) + ",";
    json += "\"field_power\":" + fieldpower::statusJson(battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant)) + ",";
    json += "\"wifi_off_pending\":" + String(devicesetup::wifiOffPending() ? "true" : "false") + ",";
    json += "\"wifi_suspended\":" + String(devicesetup::wifiSuspended() ? "true" : "false") + ",";
    json += "\"wifi_off_error\":\"" + jsonEscape(devicesetup::wifiOffError()) + "\",";
    json += "\"last_power_test\":" + fieldPowerTestJson() + ",";
    json += "\"recording\":" + recording::statusJson(recordingHardwareSupported(), s3BoardVariant, currentSampleRate) + ",";
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
    json += "\"board_id\":\"" + jsonEscape(String(FW_BOARD_ID_STR)) + "\",";
    json += "\"board_name\":\"" + jsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"chip_family\":\"" + jsonEscape(String(FW_CHIP_FAMILY_STR)) + "\",";
    json += "\"ota_artifact\":\"" + jsonEscape(String(FW_OTA_ARTIFACT_STR)) + "\",";
    json += "\"ota_auto_supported\":" + String(FW_OTA_ARTIFACT_STR[0] ? "true" : "false") + ",";
    json += "\"ip\":\"" + controlIp + "\",";
    json += "\"stream_url_ip\":\"rtsp://" + controlIp + ":8554/audio1\",";
    json += "\"stream_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio1\",";
    json += "\"stream1_url_ip\":\"rtsp://" + controlIp + ":8554/audio1\",";
    json += "\"stream2_url_ip\":\"rtsp://" + controlIp + ":8554/audio2\",";
    json += "\"stream1_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio1\",";
    json += "\"stream2_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio2\",";
    json += "\"stream1_target\":" + String((uint32_t)streamProfiles[0].target) + ",";
    json += "\"stream2_target\":" + String((uint32_t)streamProfiles[1].target) + ",";
    json += "\"stream1_enabled\":" + String(streamEnabled[0]?"true":"false") + ",";
    json += "\"stream2_enabled\":" + String(streamEnabled[1]?"true":"false") + ",";
    json += "\"max_clients\":" + String((uint32_t)maxActiveClients) + ",";
    // Per-stream client counts
    uint8_t s1clients = 0, s2clients = 0;
    getStreamClientCounts(s1clients, s2clients);
    json += "\"s1_clients\":" + String(s1clients) + ",";
    json += "\"s1_streaming\":" + String(streamStats[0].streaming?"true":"false") + ",";
    json += "\"s1_pkt_rate\":" + String(getCurrentPacketRate(0)) + ",";
    json += "\"s1_last_play\":\"" + jsonEscape(formatSince(streamStats[0].lastPlayMs)) + "\",";
    json += "\"s2_clients\":" + String(s2clients) + ",";
    json += "\"s2_streaming\":" + String(streamStats[1].streaming?"true":"false") + ",";
    json += "\"s2_pkt_rate\":" + String(getCurrentPacketRate(1)) + ",";
    json += "\"s2_last_play\":\"" + jsonEscape(formatSince(streamStats[1].lastPlayMs)) + "\",";
    json += "\"mdns_hostname\":\"" + jsonEscape(mdnsHostname) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(freeHeap/1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap/1024) + ",";
    const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    json += "\"internal_heap_free_kb\":" + String((uint32_t)(heap_caps_get_free_size(internalCaps)/1024)) + ",";
    json += "\"internal_heap_largest_kb\":" + String((uint32_t)(heap_caps_get_largest_free_block(internalCaps)/1024)) + ",";
    json += "\"internal_heap_min_kb\":" + String((uint32_t)(heap_caps_get_minimum_free_size(internalCaps)/1024)) + ",";
    json += "\"psram_total_kb\":" + String(ESP.getPsramSize()/1024) + ",";
    json += "\"psram_free_kb\":" + String(ESP.getFreePsram()/1024) + ",";
    json += "\"uptime\":\"" + uptimeStr + "\",";
    json += "\"time_synced\":" + String(timeSynced?"true":"false") + ",";
    json += "\"time_valid\":" + String(localTimeStr != "unavailable" && utcTimeStr != "unavailable" ? "true" : "false") + ",";
    json += "\"time_sync_status\":\"" + String(timeSyncStatusCode()) + "\",";
    json += "\"time_sync_enabled\":" + String(timeSyncEnabled?"true":"false") + ",";
    json += "\"last_time_sync\":\"" + jsonEscape(timeSynced ? formatSince(lastTimeSyncSuccess) : String("never")) + "\",";
    json += "\"local_time\":\"" + jsonEscape(localTimeStr) + "\",";
    json += "\"utc_time\":\"" + jsonEscape(utcTimeStr) + "\",";
    json += "\"time_offset_min\":" + String(timeOffsetMinutes) + ",";
    json += "\"effective_time_offset_min\":" + String(effectiveTimeOffsetMinutes()) + ",";
    json += "\"time_zone_mode\":" + String((uint32_t)timeZoneMode) + ",";
    json += "\"time_zone_rule\":\"" + jsonEscape(customTimeZoneRule) + "\",";
    json += "\"mdns_enabled\":" + String(mdnsEnabled?"true":"false") + ",";
    json += "\"mqtt_enabled\":" + String(mqttEnabled?"true":"false") + ",";
    json += "\"mqtt_connected\":" + String(mqttConnected?"true":"false") + ",";
    json += "\"mqtt_host\":\"" + jsonEscape(mqttHost) + "\",";
    json += "\"mqtt_port\":" + String((uint32_t)mqttPort) + ",";
    json += "\"mqtt_user\":\"" + jsonEscape(mqttUser) + "\",";
    json += "\"mqtt_topic\":\"" + jsonEscape(mqttTopicPrefix) + "\",";
    json += "\"mqtt_discovery\":\"" + jsonEscape(mqttDiscoveryPrefix) + "\",";
    json += "\"mqtt_client_id\":\"" + jsonEscape(mqttClientId) + "\",";
    json += "\"mqtt_interval_sec\":" + String((uint32_t)mqttPublishIntervalSec) + ",";
    json += "\"mqtt_last_error\":\"" + jsonEscape(mqttLastError) + "\",";
    json += "\"mqtt_discovery_status\":\"" + String(!mqttEnabled ? "disabled" : !mqttConnected ? "waiting for connection" : mqttDiscoveryPublished ? "published" : "pending / retrying") + "\",";
    bool schedTimeValid = false;
    bool schedAllowNow = isStreamScheduleAllowedNow(&schedTimeValid);
    json += "\"stream_schedule_enabled\":" + String(streamScheduleEnabled?"true":"false") + ",";
    json += "\"stream_schedule_start_min\":" + String(streamScheduleStartMin) + ",";
    json += "\"stream_schedule_stop_min\":" + String(streamScheduleStopMin) + ",";
    json += "\"stream_schedule_allow_now\":" + String(schedAllowNow?"true":"false") + ",";
    json += "\"stream_schedule_time_valid\":" + String(schedTimeValid?"true":"false") + ",";
    json += "\"deep_sleep_sched_enabled\":" + String(deepSleepScheduleEnabled?"true":"false") + ",";
    json += "\"deep_sleep_status_code\":\"" + jsonEscape(deepSleepStatusCode) + "\",";
    json += "\"deep_sleep_next_sec\":" + String(deepSleepNextSleepSec) + ",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled?"true":"false") + ",";
    json += "\"client\":\"" + jsonEscape(getRtspClientSummary()) + "\",";
    json += "\"streaming\":" + String(isStreaming?"true":"false") + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"last_rtsp_connect\":\"" + jsonEscape(formatSince(lastRtspClientConnectMs)) + "\",";
    json += "\"last_stream_start\":\"" + jsonEscape(formatSince(lastRtspPlayMs)) + "\"";
    json += "}";
    apiSendJSON(json);
}

static void appendAudioDiagnosticsJson(String& json) {
    uint32_t loadSnapshot = audioProcessingLoadSnapshot.load(std::memory_order_relaxed);
    uint32_t lastCaptureMs = audioProducerLastReadMs;
    bool loadValid = audioProducerRunning && !audioProducerStopRequested &&
                     uint32_t(millis() - lastCaptureMs) < 2000 &&
                     (loadSnapshot & AudioProcessingWindow::validBit);
    json += "\"audio_cpu_valid\":" + String(loadValid ? "true" : "false") + ",";
    json += "\"audio_cpu_pct\":" + (loadValid ? String(AudioProcessingWindow::currentX10(loadSnapshot) / 10.0f, 1) : String("null")) + ",";
    json += "\"audio_cpu_peak_pct\":" + (loadValid ? String(AudioProcessingWindow::peakX10(loadSnapshot) / 10.0f, 1) : String("null")) + ",";
    DmaOverrunStatus dma = getI2sDmaStatus();
    json += "\"dma_overruns_available\":" + String(dma.available ? "true" : "false") + ",";
    json += "\"dma_overruns_total\":" + (dma.available ? String(dma.total) : String("null")) + ",";
    json += "\"dma_overruns_1min\":" + (dma.available ? String(dma.recent) : String("null")) + ",";
    String dmaAge = dma.hasEvent ? formatUptime(uint32_t(millis() - dma.lastObservedMs) / 1000) + " ago" : String("never");
    json += "\"dma_overrun_last_age\":\"" + jsonEscape(dmaAge) + "\",";
}

static void httpAudioStatus() {
    float latency_ms = (float)currentBufferSize / currentSampleRate * 1000.0f;
    String json = "{";
    json.reserve(1500);
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"gain\":" + String(currentGainFactor,2) + ",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C5)
    json += "\"buffer_size_recommended\":512,";
#else
    json += "\"buffer_size_recommended\":0,";
#endif
    json += "\"audio_dsp\":\"integer\",";
    json += "\"i2s_shift\":" + String(i2sShiftBits) + ",";
    json += "\"noise\":" + noiseshadow::statusJson(false) + ",";
    json += "\"mic_format\":" + String(micFormat) + ",";
    json += "\"s3_board_supported\":" + String(s3BoardSelectionSupported() ? "true" : "false") + ",";
    json += "\"s3_board\":" + String(s3BoardVariant) + ",";
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    json += "\"audio_codec\":\"ES8311\",";
#endif
    json += "\"builtin_mic\":" + String(builtinMicSelected() ? "true" : "false") + ",";
    json += "\"pdm_supported\":" + String(pdmMicSupported() ? "true" : "false") + ",";
    json += "\"pdm_decoder\":\"" + String(pdmDecoderName()) + "\",";
    json += "\"pdm_clock_hz\":" + String(micFormat == 2 ? 3072000 : 0) + ",";
    json += "\"pdm_word_stride\":" + String(pdmRawWordStride()) + ",";
    json += "\"pdm_bit_order\":\"" + String(pdmRawBitOrder()) + "\",";
    json += "\"latency_ms\":" + String(latency_ms,1) + ",";
    extern bool highpassEnabled; extern uint16_t highpassCutoffHz;
    json += "\"profile\":\"" + jsonEscape(profileName(currentBufferSize)) + "\",";
    json += "\"hp_enable\":" + String(highpassEnabled?"true":"false") + ",";
    json += "\"hp_cutoff_hz\":" + String((uint32_t)highpassCutoffHz) + ",";
    // Metering/clipping
    uint16_t p = (peakHoldAbs16 > 0) ? peakHoldAbs16 : lastPeakAbs16;
    float peak_pct = (p <= 0) ? 0.0f : (100.0f * (float)p / 32767.0f);
    float peak_dbfs = (p <= 0) ? -90.0f : (20.0f * log10f((float)p / 32767.0f));
    json += "\"peak_pct\":" + String(peak_pct,1) + ",";
    json += "\"peak_dbfs\":" + String(peak_dbfs,1) + ",";
    json += "\"clip\":" + String(audioClippedLastBlock?"true":"false") + ",";
    json += "\"clip_count\":" + String(audioClipCount) + ",";
    json += "\"producer_running\":" + String(audioProducerRunning?"true":"false") + ",";
    appendAudioDiagnosticsJson(json);
    bool capturing = audioProducerRunning && !audioProducerStopRequested;
    json += "\"mic_signal\":\"" + String(MicrophoneSignal::name(microphoneSignal.state(millis(), capturing))) + "\",";
    json += "\"mic_flatline_sec\":" + String(microphoneSignal.flatSeconds(millis(), capturing)) + ",";
    json += "\"browser_listening\":" + String(browserAudioActive() ? "true" : "false") + ",";
    json += "\"i2s_error_count\":" + String(audioI2SErrorCount) + ",";
    json += "\"rb_capacity_bytes\":" + String((uint32_t)audioRingBufferCapacityBytes) + ",";
    json += "\"rb_in_psram\":" + String(audioRingInPsram ? "true" : "false") + ",";
    json += "\"listen_queue_bytes\":" + String((uint32_t)browserAudioQueueBytes()) + ",";
    const auto listen = browserAudioDiagnostics();
    json += "\"listen_last_stop\":\"" + String(listen.lastStop) + "\",";
    json += "\"listen_stops\":" + String(listen.stops) + ",";
    json += "\"listen_overflows\":" + String(listen.overflows) + ",";
    json += "\"listen_send_timeouts\":" + String(listen.sendTimeouts) + ",";
    json += "\"listen_no_audio_timeouts\":" + String(listen.noAudioTimeouts) + ",";
    json += "\"listen_max_send_stall_ms\":" + String(listen.maxSendStallMs) + ",";
    json += "\"listen_send_recoveries\":" + String(listen.sendRecoveries) + ",";
    json += "\"listen_socket_errors\":" + String(listen.socketErrors) + ",";
    json += "\"listen_max_loop_gap_ms\":" + String(listen.maxLoopGapMs) + ",";
    json += "\"listen_bytes_sent\":" + String(listen.bytesSent) + ",";
    json += "\"listen_independent_worker\":" + String(listen.independentWorker ? "true" : "false") + ",";
    json += "\"listen_worker_stack_free_bytes\":" + String(listen.workerStackFreeBytes) + ",";
    json += "\"listen_preview_discarded_bytes\":" + String(listen.previewDiscardedBytes) + ",";
    json += "\"rb_chunks\":" + String(audioRingBufferChunkCount) + ",";
    json += "\"rb_drops\":" + String(audioRingBufferDropCount.load(std::memory_order_relaxed)) + ",";
    json += "\"rb_flushes\":" + String(audioRingBufferFlushCount) + ",";
    json += "\"rtsp_write_stalls\":" + String(rtspWriteStallCount) + ",";
    json += "\"rtsp_write_timeouts\":" + String(rtspWriteTimeoutCount) + ",";
    json += "\"rtsp_write_failures\":" + String(rtspWriteFailCount) + ",";
    json += "\"rtsp_client_teardowns\":" + String(rtspTeardownCount) + ",";
    json += "\"rtsp_client_disconnects\":" + String(rtspDisconnectCount) + ",";
    json += "\"last_stop_reason\":\"" + jsonEscape(lastStreamStopReason) + "\",";
    json += "\"last_stop_time\":\"" + jsonEscape(lastStreamStopTime) + "\",";
    json += "\"last_stop_stream\":" + String(lastStreamStopStream) + ",";
    json += "\"last_stop_age\":\"" + jsonEscape(lastStreamStopTime.length() ? formatSince(lastStreamStopMs) : String("never")) + "\"";
    json += "}";
    apiSendJSON(json);
}

static void httpPerfStatus() {
    String json = "{";
    json.reserve(220);
    json += "\"restart_threshold_pkt_s\":" + String(minAcceptableRate) + ",";
    json += "\"check_interval_min\":" + String(performanceCheckInterval) + ",";
    json += "\"auto_recovery\":" + String(autoRecoveryEnabled?"true":"false") + ",";
    json += "\"auto_threshold\":" + String(autoThresholdEnabled?"true":"false") + ",";
    json += "\"recommended_min_rate\":" + String(computeRecommendedMinRate()) + ",";
    json += "\"scheduled_reset\":" + String(scheduledResetEnabled?"true":"false") + ",";
    json += "\"reset_hours\":" + String(resetIntervalHours) + "}";
    apiSendJSON(json);
}

static void httpThermal() {
    String since = "";
    if (overheatTripTemp > 0.0f && overheatTriggeredAt != 0) {
        since = formatSince(overheatTriggeredAt);
    }
    bool autoPending = thermalRecovery.autoPending(overheatAutoResumeEnabled, overheatProtectionEnabled);
    bool manualRequired = overheatLatched && !autoPending;
    String json = "{";
    json.reserve(1050);
    if (lastTemperatureValid) {
        json += "\"current_c\":" + String(lastTemperatureC,1) + ",";
    } else {
        json += "\"current_c\":null,";
    }
    json += "\"current_valid\":" + String(lastTemperatureValid?"true":"false") + ",";
    json += "\"max_c\":" + String(maxTemperature,1) + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + ",";
    json += "\"cpu_max_mhz\":" + String(maxSupportedCpuFrequencyMhz()) + ",";
    json += "\"protection_enabled\":" + String(overheatProtectionEnabled?"true":"false") + ",";
    json += "\"shutdown_c\":" + String(overheatShutdownC,0) + ",";
    json += "\"latched\":" + String(overheatLockoutActive?"true":"false") + ",";
    json += "\"latched_persist\":" + String(overheatLatched?"true":"false") + ",";
    json += "\"sensor_fault\":" + String(overheatSensorFault?"true":"false") + ",";
    json += "\"last_trip_c\":" + String(overheatTripTemp,1) + ",";
    json += "\"last_reason\":\"" + jsonEscape(overheatLastReason) + "\",";
    json += "\"last_trip_ts\":\"" + jsonEscape(overheatLastTimestamp) + "\",";
    json += "\"last_trip_since\":\"" + jsonEscape(since) + "\",";
    json += "\"auto_resume_enabled\":" + String(overheatAutoResumeEnabled?"true":"false") + ",";
    json += "\"auto_resume_pending\":" + String(autoPending?"true":"false") + ",";
    json += "\"recording_mode\":" + String(recording::mode()?"true":"false") + ",";
    json += "\"recording_resume_pending\":" + String(recording::resumePending()?"true":"false") + ",";
    json += "\"manual_blocked\":" + String(thermalRecovery.state.manualBlocked?"true":"false") + ",";
    json += "\"emergency\":" + String(thermalRecovery.state.emergency?"true":"false") + ",";
    json += "\"notice\":" + String(thermalRecovery.state.notice?"true":"false") + ",";
    json += "\"auto_resumed\":" + String(thermalRecovery.state.resumed?"true":"false") + ",";
    json += "\"trips_24h\":" + String(thermalRecovery.state.count) + ",";
    json += "\"cool_limit_c\":" + String(overheatLatched ? thermalRecovery.state.coolLimitC : overheatShutdownC - ThermalRecovery::COOL_MARGIN_C, 1) + ",";
    json += "\"cool_remaining_sec\":" + String(thermalRecovery.remainingSec(millis())) + ",";
    json += "\"can_acknowledge\":" + String((thermalRecovery.state.notice && thermalRecovery.canAcknowledge(millis(), overheatProtectionEnabled))?"true":"false") + ",";
    json += "\"schedule_allowed\":" + String((recording::mode() || isStreamScheduleAllowedNow(nullptr))?"true":"false") + ",";
    json += "\"manual_restart\":" + String(manualRequired?"true":"false");
    json += "}";
    apiSendJSON(json);
}

static void httpThermalClear() {
    if (!requireMutationAuth()) return;

    if (!thermalRecovery.state.notice) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"no_thermal_notice\"}"));
        return;
    }
    if (!acknowledgeThermalShutdown()) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"cooling_required\"}"));
        return;
    }
    webui_pushLog(F("UI action: thermal_acknowledge"));
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpLogs() {
    web.sendHeader("Cache-Control","no-cache");
    web.sendHeader("X-ESP32MIC-Log-Capacity",String(logBuffer.capacity()));
    web.sendHeader("X-ESP32MIC-Log-Storage",logBuffer.usesPsram()?"psram":"internal");
    if(web.hasArg("page")) {
        uint64_t cursor=0;
        const bool before=web.hasArg("before"),after=web.hasArg("after");
        if(web.arg("page")!="1" || (before&&after) || ((before||after)&&!DeviceLog::parseCursor(web.arg(after?"after":"before").c_str(),cursor))) {
            web.send(400,"text/plain","Invalid log page cursor");return;
        }
        char bytes[DeviceLog::kPageBytes+1];
        const auto page=logBuffer.page(bytes,DeviceLog::kPageBytes,after,before||after,cursor);
        bytes[page.bytes]=0;
        web.sendHeader("X-ESP32MIC-Log-First",String((unsigned long long)page.first));
        web.sendHeader("X-ESP32MIC-Log-Last",String((unsigned long long)page.last));
        web.sendHeader("X-ESP32MIC-Log-Start",String((unsigned long long)page.start));
        web.sendHeader("X-ESP32MIC-Log-End",String((unsigned long long)page.end));
        web.sendHeader("X-ESP32MIC-Log-Moved",page.cursorMoved?"1":"0");
        web.send(200,"text/plain; charset=utf-8",String(bytes));return;
    }
    if(web.hasArg("download"))web.sendHeader("Content-Disposition","attachment; filename=\"esp32mic-log.txt\"");
    // Legacy read/download serves the whole retained log in bounded chunks.
    // No temporary 32 KiB String is ever built in internal RAM.
    web.setContentLength(logBuffer.size());
    web.send(200,"text/plain; charset=utf-8","");
    char bytes[1024];
    for(size_t offset=0;offset<logBuffer.size();) {
        const size_t count=logBuffer.copy(offset,bytes,sizeof(bytes));
        web.sendContent(bytes,count);offset+=count;
    }
}

static void httpActionServerStart(){
    if (!requireMutationAuth()) return;

    if (recording::mode()) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"Switch to Streaming mode to start RTSP\"}"));
        return;
    }
    if (overheatLatched) {
        webui_pushLog(F("Server start blocked: thermal protection latched"));
        apiSendJSON(F("{\"ok\":false,\"error\":\"thermal_latched\"}"));
        return;
    }
    if (!rtspServerEnabled) {
        rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true);
        overheatLockoutActive = false;
        overheatConsecutiveSamples = 0;
    }
    webui_pushLog(F("UI action: server_start"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionServerStop(){
    if (!requireMutationAuth()) return;

    cancelThermalResume();
    rtspServerEnabled=false; stopAllRtspClients("Web UI RTSP server disabled"); rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: reset_i2s"));
    if (restartI2S()) apiSendJSON(F("{\"ok\":true}"));
    else apiSendJSON(F("{\"ok\":false,\"error\":\"i2s_restart_failed\"}"));
}

static void httpActionTimeSync(){
    if (!requireMutationAuth()) return;

    const bool accepted = attemptTimeSync(true, true);
    apiSendJSON(accepted ? F("{\"ok\":true,\"pending\":true}") :
        F("{\"ok\":false,\"error\":\"Enable Time Sync and connect to Wi-Fi first.\"}"));
}

static bool parseBssidStr(const String &s, uint8_t out[6]) {
    if (s.length() != 17) return false;
    for (int i = 0; i < 6; ++i) {
        int p = i * 3;
        if (i < 5) {
            char sep = s[p + 2];
            if (sep != ':' && sep != '-') return false;
        }
        int nibble[2];
        for (int k = 0; k < 2; ++k) {
            char c = s[p + k];
            if (c >= '0' && c <= '9')      nibble[k] = c - '0';
            else if (c >= 'a' && c <= 'f') nibble[k] = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nibble[k] = c - 'A' + 10;
            else return false;
        }
        out[i] = (uint8_t)((nibble[0] << 4) | nibble[1]);
    }
    return true;
}

// On-demand asynchronous scan: polling never starts another scan.
static bool wifiApScanRequested = false;
static bool wifiApScanQueued = false;
static unsigned long wifiApScanStarted = 0;
static bool wifiApScanAll = false;

static void httpWifiApStatus() {
    String json = "{\"ok\":true,\"ssid\":\"" + jsonEscape(wifiApSsid()) +
        "\",\"locked_bssid\":\"" + wifiApPinnedText() +
        "\",\"connected\":" + (WiFi.status() == WL_CONNECTED ? "true" : "false") +
        ",\"bssid\":\"" + WiFi.BSSIDstr() + "\",\"rssi\":" + String(WiFi.RSSI()) +
        ",\"recovery\":" + (wifiApRecoveryActive() ? "true" : "false") + "}";
    apiSendJSON(json);
}

static void httpWifiApScanStart() {
    if (!requireMutationAuth()) return;
    if (wifiApScanQueued || wifiReconnectAt != 0 || devicesetup::connectionBusy() || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"busy\"}"));
        return;
    }
    String ssid = wifiApSsid();
    wifiApScanAll = web.uri() == "/api/setup/scan";
    if (!wifiApScanAll && !ssid.length()) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"no_ssid\"}"));
        return;
    }
    // Acknowledge while the radio is still on the AP channel. Scanning can
    // temporarily interrupt the captive client's connection, especially on C5.
    wifiApScanRequested = true;
    wifiApScanQueued = true;
    wifiApScanStarted = millis();
    apiSendJSON(F("{\"ok\":true}"));
}

static void wifiApScanLoop() {
    if (!wifiApScanQueued || millis() - wifiApScanStarted < 300UL) return;
    wifiApScanQueued = false;
    WiFi.scanDelete();
    String ssid = wifiApSsid();
    if (wifiApScanAll) {
        // Match WiFiManager's default portal scan: synchronous, visible SSIDs,
        // Arduino defaults. Do not interleave connection management or HTTP
        // requests while the single radio is scanning both C5 bands.
        WiFi.scanNetworks(false, false);
        if (devicesetup::apActive() && WiFi.status() != WL_CONNECTED &&
            !devicesetup::connectionBusy()) WiFi.enableSTA(false);
    } else {
        WiFi.scanNetworks(true, true, false, 120, 0, ssid.c_str());
    }
}

static void httpWifiApScanResults() {
    if (!wifiApScanRequested || millis() - wifiApScanStarted > 60000UL) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"scan_expired\"}"));
        return;
    }
    int16_t count = WiFi.scanComplete();
    if (wifiApScanQueued || count == WIFI_SCAN_RUNNING) {
        apiSendJSON(F("{\"ok\":true,\"scanning\":true}"));
        return;
    }
    if (count < 0) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"scan_failed\"}"));
        return;
    }
    std::vector<int> matches;
    String ssid = wifiApSsid();
    for (int i = 0; i < count && i < 256; ++i) {
        if (wifiApScanAll || WiFi.SSID(i) == ssid) matches.push_back(i);
    }
    std::sort(matches.begin(), matches.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
    String json = "{\"ok\":true,\"scanning\":false,\"aps\":[";
    size_t limit = std::min(matches.size(), size_t(32));
    for (size_t j = 0; j < limit; ++j) {
        int i = matches[j];
        if (j) json += ',';
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"secure\":" +
            String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") +
            ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]}";
    apiSendJSON(json);
}

static void httpWifiApSave() {
    if (!requireMutationAuth()) return;
    if (!web.hasArg("bssid")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"missing_bssid\"}"));
        return;
    }
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING || wifiReconnectAt != 0) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"busy\"}"));
        return;
    }
    String value = web.arg("bssid");
    value.trim();
    uint8_t bssid[6] = {};
    if (value.length() && (!parseBssidStr(value, bssid) || (bssid[0] & 1) ||
        !(bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]))) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"bad_bssid\"}"));
        return;
    }
    if (!wifiApSave(value.length() ? bssid : nullptr)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"save_failed\"}"));
        return;
    }
    webui_pushLog(value.length() ? F("WiFi AP selection saved: locked") : F("WiFi AP selection saved: automatic"));
    scheduleWifiReconnect(wifiApPinned(), 500);
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionWifiReconnect(){
    if (!requireMutationAuth()) return;

    uint8_t bssid[6];
    bool hasBssid = false;
    if (web.hasArg("bssid")) {
        String v = web.arg("bssid");
        v.trim();
        if (v.length() > 0) {
            if (!parseBssidStr(v, bssid)) {
                apiSendJSON(F("{\"ok\":false,\"error\":\"bad_bssid\"}"));
                return;
            }
            hasBssid = true;
        }
    }
    if (hasBssid) {
        webui_pushLog(F("UI action: wifi_reconnect (BSSID pinned)"));
    } else {
        webui_pushLog(F("UI action: wifi_reconnect"));
    }
    scheduleWifiReconnect(hasBssid ? bssid : nullptr, 300);
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionNetworkReset(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: network_reset (clearing Wi-Fi and rebooting)"));
    if (!wifiApSave(nullptr)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"save_failed\"}"));
        return;
    }
    if (!devicesetup::reset(false)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"save_failed\"}"));
        return;
    }
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(false, 800);
}

static void httpActionMqttDiscovery(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: mqtt_discovery"));
    mqttPublishDiscoverySoon();
    apiSendJSON(F("{\"ok\":true}"));
}

static bool valueArgTrimmed(String& out) {
    if (!web.hasArg("value")) return false;
    out = web.arg("value");
    out.trim();
    return out.length() > 0;
}

using settingsvalidation::parseUInt32Strict;
using settingsvalidation::parseInt32Strict;
using settingsvalidation::parseFloatStrict;

static inline bool argToFloat(float &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseFloatStrict(v, out);
}
static inline bool argToUInt(uint32_t &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseUInt32Strict(v, out);
}
static inline bool argToUShort(uint16_t &out) {
    uint32_t v = 0;
    if (!argToUInt(v) || v > 65535u) return false;
    out = (uint16_t)v;
    return true;
}
static inline bool argToUChar(uint8_t &out) {
    uint32_t v = 0;
    if (!argToUInt(v) || v > 255u) return false;
    out = (uint8_t)v;
    return true;
}
static inline bool argToInt(int32_t &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseInt32Strict(v, out);
}

static bool applyAndSaveAudio(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift) {
    if (applyAudioConfig(newRate, newGain, newBuffer, newShift)) {
        saveAudioSettings();
        return true;
    }
    return false;
}

static bool applyAndSaveMicFormat(uint8_t newFormat) {
    if (applyMicFormatConfig(newFormat)) {
        saveAudioSettings();
        return true;
    }
    return false;
}

static void httpMicProfile() {
    uint32_t format;
    micprofiles::Settings profile;
    if (!parseUInt32Strict(web.arg("format"), format) || format > 2 ||
        !getRecommendedMicSettings(uint8_t(format), profile) ||
        (format == 2 && !pdmMicSupported())) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_value\"}"));
        return;
    }
    String json;
    json.reserve(480);
    json = "{\"ok\":true,\"board\":\"" + jsonEscape(String(FW_BOARD_NAME_STR)) +
        "\",\"mic_format\":" + String(format) + ",\"current_format\":" + String(micFormat) +
        ",\"sample_rate\":" + String(profile.rate) + ",\"gain\":" + String(profile.gain, 2) +
        ",\"buffer_size\":" + String(profile.buffer) + ",\"i2s_shift\":" + String(profile.shift) +
        ",\"hp_enable\":" + String(profile.highpass ? "true" : "false") +
        ",\"hp_cutoff_hz\":" + String(profile.cutoff) + ",\"cpu_mhz\":" + String(profile.cpu) + "}";
    apiSendJSON(json);
}

static void httpSaveMicProfile() {
    if (!requireMutationAuth()) return;
    uint32_t format;
    const String mode = web.arg("settings");
    if (!parseUInt32Strict(web.arg("format"), format) || format > 2 ||
        (mode != "recommended" && mode != "keep")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_value\"}"));
        return;
    }
    if (!applyMicProfileConfig(uint8_t(format), mode == "recommended")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"i2s_restart_failed\"}"));
        return;
    }
    saveAudioSettings();
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpSaveS3Board() {
    if (!requireMutationAuth()) return;
    uint32_t variant;
    if (!s3BoardSelectionSupported() || !parseUInt32Strict(web.arg("variant"), variant) || variant > 2) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_value\"}"));
        return;
    }
    if (!applyS3BoardConfig(uint8_t(variant))) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"i2s_restart_failed\"}"));
        return;
    }
    saveAudioSettings();
    if (recording::mode()) recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    apiSendJSON(F("{\"ok\":true}"));
}

// Validate the whole group before changing runtime state or writing preferences.
static void httpSaveSchedule() {
    if (!requireMutationAuth()) return;
    uint32_t start, stop;
    String enabled = web.arg("enabled"), sleep = web.arg("sleep");
    if ((enabled != "on" && enabled != "off") || (sleep != "on" && sleep != "off") ||
        !parseUInt32Strict(web.arg("start"), start) || start > 1439 ||
        !parseUInt32Strict(web.arg("stop"), stop) || stop > 1439 ||
        !settingsvalidation::validSchedule(enabled == "on", sleep == "on", start, stop)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_schedule\"}"));
        return;
    }
    streamScheduleStartMin = (uint16_t)start;
    streamScheduleStopMin = (uint16_t)stop;
    streamScheduleEnabled = enabled == "on";
    deepSleepScheduleEnabled = sleep == "on";
    saveAudioSettings();
    webui_pushLog(F("UI: streaming schedule saved"));
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpSaveMqtt() {
    if (!requireMutationAuth()) return;
    String enabled = web.arg("enabled"), host = web.arg("host"), user = web.arg("user");
    String password = web.arg("password"), clear = web.arg("clear_password");
    host.trim(); user.trim();
    uint32_t port;
    if (!web.hasArg("host") || !web.hasArg("user") ||
        (enabled != "on" && enabled != "off") || (enabled == "on" && !host.length()) ||
        host.length() > 96 || user.length() > 96 || password.length() > 128 ||
        !parseUInt32Strict(web.arg("port"), port) || port < 1 || port > 65535 ||
        (clear != "on" && clear != "off") || (clear == "on" && password.length())) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_mqtt\"}"));
        return;
    }
    mqttEnabled = enabled == "on";
    mqttHost = host; mqttPort = (uint16_t)port; mqttUser = user;
    if (clear == "on" || password.length()) mqttPassword = password;
    saveAudioSettings();
    mqttRequestReconnect(true);
    webui_pushLog(F("UI: MQTT connection saved"));
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpSet() {
    if (!requireMutationAuth()) return;

    if (!web.hasArg("key")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"missing_key\"}"));
        return;
    }

    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (key == "mqtt_pass") {
        webui_pushLog(F("UI set: mqtt_pass=<hidden>"));
    } else if (val.length()) {
        webui_pushLog(String("UI set: ")+key+"="+val);
    }

    bool handled = false;
    bool applied = false;

    if (key == "gain") {
        handled = true;
        float v;
        if (argToFloat(v) && v >= 0.1f && v <= 100.0f) {
            applied = applyAndSaveAudio(currentSampleRate, v, currentBufferSize, i2sShiftBits);
        }
    }
    else if (key == "rate") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 8000 && v <= 192000) {
            applied = applyAndSaveAudio(v, currentGainFactor, currentBufferSize, i2sShiftBits);
        }
    }
    else if (key == "buffer") {
        handled = true;
        uint16_t v;
        if (argToUShort(v) && v >= 256 && v <= 8192) {
            applied = applyAndSaveAudio(currentSampleRate, currentGainFactor, v, i2sShiftBits);
        }
    }
    else if (key == "shift") {
        handled = true;
        uint8_t v;
        if (argToUChar(v) && v <= 24) {
            applied = applyAndSaveAudio(currentSampleRate, currentGainFactor, currentBufferSize, v);
        }
    }
    else if (key == "mic_format") {
        handled = true;
        uint8_t v;
        if (argToUChar(v) && v <= 2) {
            applied = applyAndSaveMicFormat(v);
        }
    }
    else if (key == "wifi_tx") {
        handled = true;
        float v;
        if (argToFloat(v) && v >= -1.0f && v <= 19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm = snapWifiTxDbm(v); applyWifiTxPower(true); saveAudioSettings(); applied = true; }
    }
    else if (key == "auto_recovery") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { autoRecoveryEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "thr_mode") {
        handled = true;
        String v = web.arg("value");
        if (v == "auto") { autoThresholdEnabled = true; minAcceptableRate = computeRecommendedMinRate(); saveAudioSettings(); applied = true; }
        else if (v == "manual") { autoThresholdEnabled = false; saveAudioSettings(); applied = true; }
    }
    else if (key == "min_rate") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 5 && v <= 200) { minAcceptableRate = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "check_interval") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 60) { performanceCheckInterval = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "sched_reset") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { extern bool scheduledResetEnabled; scheduledResetEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "reset_hours") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 168) { extern uint32_t resetIntervalHours; resetIntervalHours = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "cpu_freq") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && cpuFrequencySupported(v) && setCpuFrequencyMhz(v)) {
            cpuFrequencyMhz = (uint16_t)v;
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "hp_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { extern bool highpassEnabled; highpassEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "hp_cutoff") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 10 && v <= maxHighpassCutoffForRate(currentSampleRate)) { extern uint16_t highpassCutoffHz; highpassCutoffHz = (uint16_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { overheatProtectionEnabled = (v == "on"); overheatConsecutiveSamples = 0; thermalRecovery.resetCooling(); overheatLockoutActive = overheatLatched; saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_auto") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            overheatAutoResumeEnabled = (v == "on");
            thermalRecovery.resetCooling();
            saveAudioSettings(); applied = true;
        }
    }
    else if (key == "oh_limit") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= OH_MIN && v <= OH_MAX) { uint32_t snapped = OH_MIN + ((v - OH_MIN) / OH_STEP) * OH_STEP; overheatShutdownC = (float)snapped; if (overheatLatched) { thermalRecovery.state.coolLimitC = std::min(thermalRecovery.state.coolLimitC, overheatShutdownC - ThermalRecovery::COOL_MARGIN_C); } thermalRecovery.resetCooling(); overheatLockoutActive = overheatLatched; overheatConsecutiveSamples = 0; saveAudioSettings(); applied = true; }
    }
    else if (key == "time_offset") {
        handled = true;
        int32_t v;
        if (argToInt(v) && v >= -720 && v <= 840) { timeOffsetMinutes = v; configureTimeService(timeSyncEnabled); saveAudioSettings(); applied = true; }
    }
    else if (key == "time_zone_mode") {
        handled = true;
        int32_t v;
        if (argToInt(v) && v >= 0 && v <= 9 &&
            (v != 9 || isValidCustomTimeZoneRule(customTimeZoneRule))) {
            timeZoneMode = (uint8_t)v;
            configureTimeService(timeSyncEnabled);
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "time_zone_rule") {
        handled = true;
        String v = web.arg("value");
        if (!v.length() && timeZoneMode != 9) v = "UTC0";
        if (isValidCustomTimeZoneRule(v) || (v == "UTC0" && timeZoneMode != 9)) {
            customTimeZoneRule = v;
            if (timeZoneMode == 9) configureTimeService(timeSyncEnabled);
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "time_sync") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            timeSyncEnabled = (v == "on");
            configureTimeService(timeSyncEnabled);
            if (timeSyncEnabled) {
                attemptTimeSync(false, true);
            }
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "stream_sched") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamScheduleEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "stream_start_min") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1439) { streamScheduleStartMin = (uint16_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream_stop_min") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1439) { streamScheduleStopMin = (uint16_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "deep_sleep_sched") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            deepSleepScheduleEnabled = (v == "on");
            if (!deepSleepScheduleEnabled) {
                deepSleepStatusCode = "disabled";
                deepSleepNextSleepSec = 0;
            }
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "mdns_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { mdnsEnabled = (v == "on"); applyMdnsSetting(); saveAudioSettings(); applied = true; }
    }
    else if (key == "mdns_hostname") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        extern String sanitizeMdnsHostname(const String &input, const String &fallback);
        String next = sanitizeMdnsHostname(v, mdnsHostname);
        if (next.length() > 0) {
            mdnsHostname = next;
            if (mdnsRunning) {
                MDNS.end();
                mdnsRunning = false;
            }
            applyMdnsSetting();
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "mqtt_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            mqttEnabled = (v == "on");
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_host") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttHost = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_port") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 65535) {
            mqttPort = (uint16_t)v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_user") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttUser = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_pass") {
        handled = true;
        String v = web.arg("value");
        if (v.length() <= 128) {
            mqttPassword = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_topic") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 128) {
            mqttTopicPrefix = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_discovery") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 128) {
            mqttDiscoveryPrefix = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_client_id") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttClientId = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_interval") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 10 && v <= 2600) {
            mqttPublishIntervalSec = (uint16_t)v;
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "stream1_target") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1) { streamProfiles[0].target = (uint8_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream2_target") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1) { streamProfiles[1].target = (uint8_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream1_enabled") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamEnabled[0] = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "stream2_enabled") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamEnabled[1] = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "max_clients") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 2) { maxActiveClients = (uint8_t)v; saveAudioSettings(); applied = true; }
    }

    if (!handled) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"unknown_key\"}"));
        return;
    }
    if (!applied) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_value\"}"));
        return;
    }
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionReboot(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: reboot"));
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(false, 600);
}

static void httpActionFactoryReset(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: factory_reset"));
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(true, 600);
}

static void httpBatterySettings() {
    if (!requireMutationAuth()) return;
    if (!battery::pins(s3BoardVariant).supported) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"Battery monitoring is unsupported on this board.\"}")); return;
    }
    const String action = web.arg("action");
    bool ok = false;
    if (action == "enable" && (web.arg("enabled") == "0" || web.arg("enabled") == "1")) {
        if (web.arg("enabled") == "0" && fieldpower::settings().guardEnabled) {
            web.send(409, "application/json", "{\"ok\":false,\"error\":\"Disable low-battery protection before disabling monitoring.\"}"); return;
        }
        ok = devicesetup::setBatteryMonitoring(web.arg("enabled") == "1");
    } else if (action == "calibrate" && devicesetup::batteryMonitoring()) {
        float reference;
        if (parseFloatStrict(web.arg("reference"), reference) && reference >= 2.5f && reference <= 4.3f)
            ok = battery::calibrate(uint32_t(reference * 1000.0f + 0.5f), s3BoardVariant);
    } else if (action == "reset") ok = battery::resetCalibration();
    if (!ok) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"Could not save. Calibration needs a fresh voltage reading and a correction within +/-10%.\"}")); return;
    }
    apiSendJSON("{\"ok\":true,\"battery\":" + battery::statusJson(devicesetup::batteryMonitoring(), s3BoardVariant) + "}");
}

static void httpSaveAntenna() {
    if (!requireMutationAuth()) return;
    if (!antenna::supported()) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"unsupported_board\"}"); return;
    }
    const String value = web.arg("antenna");
    if (value != "external" && value != "internal") {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_antenna\"}"); return;
    }
    const bool useExternal = value == "external";
    if (!antenna::save(useExternal)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"antenna_save_failed\"}")); return;
    }
    const bool restart = useExternal != antenna::external();
    apiSendJSON(String("{\"ok\":true,\"reboot_required\":") + (restart ? "true" : "false") + "}");
    if (restart) scheduleReboot(false, 800);
}

static void httpSetupConnect() {
    if (!requireMutationAuth()) return;
    const String ssid = web.arg("ssid"), password = web.arg("password");
    if (ssid.length() != strlen(ssid.c_str()) || password.length() != strlen(password.c_str()) ||
        !devicesetup::testCredentials(ssid, password)) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"Check Wi-Fi name and password, or wait for the current operation.\"}");
        return;
    }
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpRecordingConnection() {
    if (!requireMutationAuth()) return;
    const String connection = web.arg("connection");
    if ((connection != "home" && connection != "direct") ||
        (connection == "direct" && !recordingpins::modeSupported(s3BoardVariant))) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid recording connection.\"}"); return;
    }
    if (recording::resumePending() || devicesetup::connectionBusy() ||
        devicesetup::wifiSuspended() || devicesetup::wifiOffPending()) {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Wait for the current operation.\"}"); return;
    }
    if (connection == "home" && !devicesetup::savedSsid().length()) {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Set up home Wi-Fi in Device configuration first.\"}"); return;
    }
    apiSendJSON(devicesetup::setRecordingConnection(connection == "direct") ?
        F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"Could not save recording connection.\"}"));
}

static void httpSetupSave() {
    if (!requireMutationAuth()) return;
    const String mode = web.arg("mode"), connection = web.arg("connection");
    const String batteryChoice = web.arg("battery");
    uint32_t variant = s3BoardVariant;
    uint32_t microphone = micFormat;
    const bool hasMicrophone = web.hasArg("microphone");
    if ((hasMicrophone && (!parseUInt32Strict(web.arg("microphone"), microphone) || microphone > 2)) ||
        (mode != "streaming" && mode != "recording") ||
        (connection != "home" && connection != "direct") ||
        (s3BoardSelectionSupported() && (!parseUInt32Strict(web.arg("variant"), variant) || variant > 2)) ||
        ((mode == "recording" || connection == "direct") && !recordingpins::modeSupported(variant)) ||
        (batteryChoice.length() && batteryChoice != "0" && batteryChoice != "1") ||
        (batteryChoice == "1" && !battery::pins(variant).supported) ||
        (antenna::supported() && web.arg("antenna") != "external" && web.arg("antenna") != "internal")) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid board, mode or connection choice.\"}");
        return;
    }
    if (hasMicrophone && s3BoardSelectionSupported() && variant == s3boards::AtomS3U && microphone != MIC_FORMAT_PDM) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"AtomS3U uses its built-in PDM microphone.\"}"); return;
    }
    if (devicesetup::connectionBusy() || recording::resumePending()) {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Wait for the current operation.\"}"); return;
    }
    if ((mode == "streaming" || connection == "home") && !devicesetup::savedSsid().length() &&
        String(devicesetup::connectionState()) != "connected" && web.arg("allow_offline") != "1") {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Connect to Wi-Fi first, or choose Finish later.\"}"); return;
    }
    if (fieldpower::settings().guardEnabled && (batteryChoice == "0" || !battery::pins(variant).supported)) {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Disable low-battery protection before disabling monitoring.\"}"); return;
    }
    const bool first = !devicesetup::complete();
    const auto oldBoard = getS3BoardConfig();
    if (s3BoardSelectionSupported() && !applyS3BoardConfig(variant)) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"Could not apply board. Select Streaming before choosing AtomS3U.\"}")); return;
    }
    if (hasMicrophone && !applyMicFormatConfig(microphone)) {
        restoreS3BoardConfig(oldBoard);
        apiSendJSON(F("{\"ok\":false,\"error\":\"Could not apply microphone; previous settings restored.\"}")); return;
    }
    const bool boardChanged = oldBoard.variant!=s3BoardVariant || oldBoard.format!=micFormat ||
        oldBoard.rate!=currentSampleRate;
    const auto rollbackBoard = [&]() {
        if (!boardChanged) return true;
        const bool restored = restoreS3BoardConfig(oldBoard);
        saveAudioSettings();
        return restored;
    };
    if (boardChanged) saveAudioSettings(); // Hardware settings precede the completion marker.
    const bool useExternal = web.arg("antenna") == "external";
    if (antenna::supported() && !antenna::save(useExternal)) {
        const bool restored = rollbackBoard();
        apiSendJSON(restored ? F("{\"ok\":false,\"error\":\"Could not save antenna.\"}") :
            F("{\"ok\":false,\"error\":\"Could not restore audio. Restart the device.\"}")); return;
    }
    if (!devicesetup::save(mode == "recording", connection == "direct", true,
                          batteryChoice.length() ? (batteryChoice == "1" ? 1 : 0) : -1)) {
        const bool restored = rollbackBoard();
        if (antenna::supported()) antenna::save(antenna::external());
        apiSendJSON(restored ? F("{\"ok\":false,\"error\":\"Could not save setup. Check Wi-Fi and try again.\"}") :
            F("{\"ok\":false,\"error\":\"Could not restore audio. Restart the device.\"}")); return;
    }
    const bool antennaRestart = antenna::supported() && useExternal != antenna::external();
    const bool applied = !first || setDeviceMode(mode == "recording");
    // SD checking is asynchronous and marks recording busy. Start it only
    // after the setup transaction can no longer need an audio rollback.
    if (boardChanged && recording::mode() && !recording::busy())
        recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    devicesetup::currentMode(recording::mode());
    apiSendJSON(String("{\"ok\":true,\"reboot_required\":") + (antennaRestart || !applied ? "true" : "false") +
        ",\"use_ap\":" + (connection == "direct" && recording::mode() && WiFi.status() != WL_CONNECTED ? "true" : "false") + "}");
    if (antennaRestart || !applied) scheduleReboot(false, 1500);
}

static void httpDeviceMode() {
    if (!requireMutationAuth()) return;
    const String requested = web.arg("mode");
    if (requested != "streaming" && requested != "recording") {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid device mode\"}"); return;
    }
    if (recording::busy() && web.arg("stop_confirmed") != "1") {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"Confirm Stop and save before switching mode\"}"); return;
    }
    bool ok = setDeviceMode(requested == "recording");
    apiSendJSON(ok ? F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"Mode change failed; check recording status\"}"));
}
static void httpRecordingAction() {
    if (!requireMutationAuth()) return;
    const String action = web.arg("action");
    bool ok = false;
    if (action == "clock") {
        uint32_t hz = 0, block = recording::kDefaultWriteBlockBytes;
        ok = parseUInt32Strict(web.arg("hz"), hz) &&
             (!web.hasArg("block_bytes") || parseUInt32Strict(web.arg("block_bytes"), block)) &&
             recording::setStorageClock(hz, block);
    } else if (action == "diagnose") ok = recording::diagnose(recordingHardwareSupported(), s3BoardVariant);
    else if (action == "check") ok = recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    else if (action == "start") ok = startRecording();
    else if (action == "stop") {
        cancelThermalResume();
        cancelRecordingAutoStart();
        ok = recording::stop() && recording::status().state != recording::State::Error;
    } else if (action == "wifi_off") ok = requestRecordingWifiOff(0);
    else if (action == "wifi_off_test" || action == "battery_sleep_test" || action == "offline_sleep_test") {
        uint32_t seconds = 0;
        if (parseUInt32Strict(web.arg("seconds"), seconds))
            ok = action == "wifi_off_test" ? (seconds >= 30 && seconds <= 300 && requestRecordingWifiOff(seconds)) :
                 action == "offline_sleep_test" ? requestOfflineSleepTest(seconds) : requestBatterySleepTest(seconds);
    } else if (action == "battery_acknowledge")
        ok = !recording::busy() && fieldpower::triggered() && fieldpower::acknowledge(battery::reading(devicesetup::batteryMonitoring(), s3BoardVariant));
    else { web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid recording action\"}"); return; }
    if (ok) { apiSendJSON(F("{\"ok\":true}")); return; }
    const auto status = recording::status();
    const bool recordingAction = action == "start" || action == "check" || action == "diagnose" || action == "stop";
    const String reason = recordingAction && action != "stop" && recording::busy() ? String("Recording is busy; wait for the current operation") :
        recordingAction && status.error[0] ? String(status.error) : String("Recording action failed; check recording status");
    apiSendJSON(String("{\"ok\":false,\"error\":\"") + jsonEscape(reason) + "\"}");
}
static void httpRecordingLogSettings() {
    if(!requireMutationAuth())return;
    uint32_t enabled,detailed,maxKiB;
    if(!parseUInt32Strict(web.arg("sd_enabled"),enabled) || enabled>1 ||
       !parseUInt32Strict(web.arg("detailed"),detailed) || detailed>1 ||
       !parseUInt32Strict(web.arg("max_kib"),maxKiB) || maxKiB<64 || maxKiB>16384) {
        web.send(400,"application/json","{\"ok\":false,\"error\":\"Invalid log settings\"}");return;
    }
    auto config=recording::logSettings();config.sdEnabled=enabled;config.detailed=detailed;config.maxKiB=maxKiB;
    const bool ok=recording::saveLogSettings(config);
    apiSendJSON(ok?F("{\"ok\":true}"):F("{\"ok\":false,\"error\":\"Could not save logging settings\"}"));
}
static void httpRecordingLogRead() {
    if(!requireMutationAuth())return;
    const String archive=web.arg("archive");
    if(archive!="0" && archive!="1") {web.send(400,"application/json","{\"ok\":false,\"error\":\"Invalid log selection\"}");return;}
    const bool ok=recording::requestLogTail(archive=="1",recordingHardwareSupported(),s3BoardVariant);
    if(ok)apiSendJSON(F("{\"ok\":true}"));
    else {
        const String error=recording::logTailError();
        apiSendJSON(String("{\"ok\":false,\"error\":\"")+jsonEscape(error.length()?error:String("Could not prepare SD log reading"))+"\"}");
    }
}
static void httpRecordingLogTail() {
    web.sendHeader("Cache-Control","no-cache");
    if(!recording::logTailReady()) {web.send(202,"text/plain","Reading SD log...");return;}
    const String error=recording::logTailError();
    if(error.length()) {web.send(503,"text/plain",error);return;}
    web.sendHeader("X-ESP32MIC-Log-Bytes",String(recording::logTailFileBytes()));
    web.send(200,"text/plain; charset=utf-8",recording::logTail());
}
static void httpFieldPowerSettings() {
    if (!requireMutationAuth()) return;
    auto config = fieldpower::settings();
    uint32_t value;
    struct Setting { const char* key; uint32_t limit; };
    const Setting fields[] = {{"cutoff_mv",4200},{"confirm_seconds",300},{"hysteresis_mv",300},
        {"wifi_delay_seconds",60},{"enabled",1},{"sleep_after_save",1}};
    bool found = false;
    for (const auto& field : fields) {
        if (!web.hasArg(field.key)) continue;
        found = true;
        if (!parseUInt32Strict(web.arg(field.key),value) || value > field.limit) {
            web.send(400,"application/json","{\"ok\":false,\"error\":\"Invalid field power settings\"}"); return;
        }
        if (!strcmp(field.key,"cutoff_mv")) config.cutoffMv=value;
        else if (!strcmp(field.key,"confirm_seconds")) config.confirmSeconds=value;
        else if (!strcmp(field.key,"hysteresis_mv")) config.hysteresisMv=value;
        else if (!strcmp(field.key,"wifi_delay_seconds")) config.wifiDelaySeconds=value;
        else if (!strcmp(field.key,"enabled")) config.guardEnabled=value;
        else config.sleepAfterSave=value;
    }
    if (!found || !fieldpower::valid(config)) {
        web.send(400,"application/json","{\"ok\":false,\"error\":\"Invalid field power settings\"}"); return;
    }
    if (config.guardEnabled && (!devicesetup::batteryMonitoring() || !battery::pins(s3BoardVariant).supported)) {
        web.send(409,"application/json","{\"ok\":false,\"error\":\"Enable battery monitoring on a supported board first.\"}"); return;
    }
    bool ok=fieldpower::saveSettings(config);
    apiSendJSON(ok ? F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"Could not save field power settings\"}"));
}

static void httpRecordingSettings() {
    if (!requireMutationAuth()) return;
    uint32_t duration = 0, segment = 0;
    const String autoStart = web.arg("auto_start");
    if (!parseUInt32Strict(web.arg("duration_minutes"), duration) || duration > 10080 ||
        !parseUInt32Strict(web.arg("segment_minutes"), segment) || segment < 1 || segment > 120 ||
        (autoStart != "0" && autoStart != "1")) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid recording settings\"}"); return;
    }
    auto config = recording::settings();
    config.durationMinutes = duration; config.segmentMinutes = segment; config.autoStart = autoStart == "1";
    bool ok = recording::saveSettings(config);
    apiSendJSON(ok ? F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"Recording settings could not be saved\"}"));
}

#if FLASH_LAYOUT_TOOL
static void httpUpdateSpaceStatus() { apiSendJSON(flashlayout::statusJson()); }
static void httpUpdateSpacePrepare() {
    if (!requireMutationAuth(true)) return;
    String error;
    if (!flashlayout::start(error)) {
        web.send(409, "application/json", String("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}");
        return;
    }
    apiSendJSON(F("{\"ok\":true}"));
}
#endif

static void httpNoiseStatus() { apiSendJSON(noiseshadow::statusJson(web.arg("spectra")=="1")); }
static void httpNoiseSave() {
    if(!requireMutationAuth()) return;
    uint32_t mode,floor,adapt;
    if(!parseUInt32Strict(web.arg("mode"),mode) || !parseUInt32Strict(web.arg("floor"),floor) ||
       !parseUInt32Strict(web.arg("adapt"),adapt) || mode>1 ||
       (floor!=6 && floor!=12 && floor!=20) || (adapt!=4 && adapt!=15 && adapt!=60)) {
        web.send(400,"application/json","{\"ok\":false,\"error\":\"invalid_value\"}"); return;
    }
    if(!noiseshadow::save(mode,floor,adapt)) {
        web.send(500,"application/json","{\"ok\":false,\"error\":\"Could not save noise reduction settings.\"}"); return;
    }
    apiSendJSON(F("{\"ok\":true}"));
}
void webui_begin() {
    web.on("/", HTTP_GET, httpIndex);
    web.on("/setup", HTTP_GET, httpSetupPage);
    web.on("/setup.css", HTTP_GET, httpSetupCss);
    web.on("/setup.js", HTTP_GET, httpSetupJs);
    web.on("/ap-password.js", HTTP_GET, httpApPasswordJs);
    web.on("/api/battery", HTTP_POST, httpBatterySettings);
    web.on("/api/field_power", HTTP_POST, httpFieldPowerSettings);
    web.on("/api/setup", HTTP_GET, httpSetupStatus);
    web.on("/api/setup", HTTP_POST, httpSetupSave);
    web.on("/api/setup/connect", HTTP_POST, httpSetupConnect);
    web.on("/api/setup/recording_connection", HTTP_POST, httpRecordingConnection);
    web.on("/api/setup/access", HTTP_POST, httpSetupAccess);
    web.on("/api/setup/scan", HTTP_POST, httpWifiApScanStart);
    web.on("/api/setup/scan", HTTP_GET, httpWifiApScanResults);
    registerOtaRoutes();
    web.on("/ota.css", HTTP_GET, httpOtaCss);
    web.on("/ota.js", HTTP_GET, httpOtaJs);
#if FLASH_LAYOUT_TOOL
    web.on("/api/update_space", HTTP_GET, httpUpdateSpaceStatus);
    web.on("/api/update_space/prepare", HTTP_POST, httpUpdateSpacePrepare);
#endif
    web.on("/api/settings/antenna", HTTP_POST, httpSaveAntenna);
    web.on("/api/device/mode", HTTP_POST, httpDeviceMode);
    web.on("/api/recording/action", HTTP_POST, httpRecordingAction);
    web.on("/api/recording/settings", HTTP_POST, httpRecordingSettings);
    web.on("/api/recording/log/settings", HTTP_POST, httpRecordingLogSettings);
    web.on("/api/recording/log/read", HTTP_POST, httpRecordingLogRead);
    web.on("/api/recording/log", HTTP_GET, httpRecordingLogTail);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/microphone_profile", HTTP_GET, httpMicProfile);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/thermal/clear", HTTP_POST, httpThermalClear);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", HTTP_POST, httpActionServerStart);
    web.on("/api/action/server_stop", HTTP_POST, httpActionServerStop);
    web.on("/api/action/reset_i2s", HTTP_POST, httpActionResetI2S);
    web.on("/api/action/time_sync", HTTP_POST, httpActionTimeSync);
    web.on("/api/wifi/ap", HTTP_GET, httpWifiApStatus);
    web.on("/api/wifi/ap", HTTP_POST, httpWifiApSave);
    web.on("/api/wifi/scan", HTTP_POST, httpWifiApScanStart);
    web.on("/api/wifi/scan", HTTP_GET, httpWifiApScanResults);
    web.on("/api/action/wifi_reconnect", HTTP_POST, httpActionWifiReconnect);
    web.on("/api/action/network_reset", HTTP_POST, httpActionNetworkReset);
    web.on("/api/action/mqtt_discovery", HTTP_POST, httpActionMqttDiscovery);
    web.on("/api/action/reboot", HTTP_POST, httpActionReboot);
    web.on("/api/action/factory_reset", HTTP_POST, httpActionFactoryReset);
    web.on("/api/set", HTTP_POST, httpSet);
    web.on("/api/settings/microphone", HTTP_POST, httpSaveMicProfile);
    web.on("/api/noise", HTTP_GET, httpNoiseStatus);
    web.on("/api/noise/set", HTTP_POST, httpNoiseSave);
    web.on("/api/settings/s3_board", HTTP_POST, httpSaveS3Board);
    web.on("/api/settings/schedule", HTTP_POST, httpSaveSchedule);
    web.on("/api/settings/mqtt", HTTP_POST, httpSaveMqtt);
    static const char* headerKeys[] = { UI_MUTATION_HEADER };
    web.collectHeaders(headerKeys, 1);
    web.onNotFound([] {
        if (devicesetup::apActive() && !web.uri().startsWith("/api/")) {
            web.sendHeader("Location", "http://192.168.4.1/");
            web.send(302, "text/plain", "Open BirdNETMic");
        } else web.send(404, "text/plain", "Not found");
    });
    web.begin();
}

void webui_handleClient() {
    wifiApScanLoop();
    // Release scan buffers even when the browser closes before collecting results.
    if (wifiApScanRequested && millis() - wifiApScanStarted > 60000UL &&
        WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        wifiApScanRequested = false;
    }
    web.handleClient();
}
