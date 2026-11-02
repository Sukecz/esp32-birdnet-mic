#pragma once
#include <Arduino.h>
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_PUBLISH_INTERVAL_SEC 60
constexpr uint16_t MQTT_PUBLISH_INTERVAL_MIN_SEC = 10;
constexpr uint16_t MQTT_PUBLISH_INTERVAL_MAX_SEC = 3600;
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
extern String mqttDeviceId;
extern bool mqttForceDiscovery;
void checkMqtt();
void mqttRequestReconnect(bool forceDiscovery);
void mqttPublishDiscoverySoon();
bool mqttPublishState(bool force);
void mqttApplyClientSettings(bool logResult);
String mqttDefaultClientId();
void mqttInitializeIdentity();
void mqttDisconnectForSleep();
String mqttDefaultTopicPrefix();
