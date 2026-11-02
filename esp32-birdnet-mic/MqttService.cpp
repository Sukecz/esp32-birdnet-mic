#include "AppRuntime.h"
#include "MqttService.h"
#include "DiscoveryClient.h"
#include "DiscoveryProgress.h"
#include "MqttDiscovery.h"
#include <PubSubClient.h>
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
static DiscoveryClient mqttNetClient;
static PubSubClient mqttClient(mqttNetClient);
static IPAddress mqttResolvedIp;
static String mqttResolvedHost = "";
static bool mqttResolvedIpValid = false;
static unsigned long lastMqttReconnectAttempt = 0;
static unsigned long lastMqttPublishMs = 0;
static unsigned long lastMqttLogMs = 0;
bool mqttDiscoveryPublished = false;
bool mqttForceDiscovery = false;
static DiscoveryProgress mqttDiscoveryProgress;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;
static const unsigned long MQTT_STREAMING_RECONNECT_INTERVAL_MS = 300000UL;
static const int32_t MQTT_STREAMING_CONNECT_TIMEOUT_MS = 500;
static const uint16_t MQTT_SOCKET_TIMEOUT_SEC = 1;


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

String mqttDefaultTopicPrefix() {
    return String("esp32mic/") + mqttDeviceMacSuffix();
}

String mqttDefaultClientId() {
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

bool mqttPublishState(bool force) {
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

void mqttApplyClientSettings(bool logResult) {
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


void mqttInitializeIdentity() {
    mqttDeviceId = sanitizeMqttClientId(String("esp32mic_") + buildMqttMacSuffix(), "esp32mic");
}
void mqttDisconnectForSleep() {
    if (!mqttClient.connected()) return;
    mqttPublishState(true);
    mqttClient.publish(mqttAvailabilityTopic().c_str(), "offline", true);
    mqttClient.loop();
    mqttClient.disconnect();
    delay(200);
}
