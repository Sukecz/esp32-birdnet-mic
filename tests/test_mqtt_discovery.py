"""Compare production discovery publication against captured pre-refactor messages."""
import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'esp32-birdnet-mic/MqttService.cpp').read_text()


def function(signature):
    start = SOURCE.index(signature + ' {')
    brace = SOURCE.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


class MqttDiscoveryTests(unittest.TestCase):
    def test_all_payloads_topics_cleanup_retention_and_failed_send(self):
        code = r'''
#include "MqttDiscovery.h"
#include "DiscoveryProgress.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using String = std::string;
String mqttDiscoveryPrefix = "custom/ha", mqttDeviceId = "DEVID";
String availability = "avail";
String mqttStateTopic() { return "state"; }
String mqttAvailabilityTopic() { return availability; }
String mqttBuildDeviceJson() { return "{\"DEV\":1}"; }
String mqttCmdRtspTopic() { return "rtsp/set"; }
String mqttCmdRebootTopic() { return "reboot"; }
String mqttCmdStreamEnabledTopic(uint8_t i) { return "enabled/" + std::to_string(i); }
String mqttCmdStreamTargetTopic(uint8_t i) { return "target/" + std::to_string(i); }
struct Message { String topic, payload; bool retained; };
struct {
    bool succeed = true;
    std::vector<Message> messages;
    bool publish(const char* topic, const char* payload, bool retained) {
        messages.push_back({topic, payload, retained}); return succeed;
    }
} mqttClient;
'''
        for signature in ['static bool mqttPublishDiscoveryConfig(const String &component, const String &objectId, const String &payload)',
                          'static String mqttDiscoveryCommandTopic(mqtt_discovery::Command command)',
                          'static bool mqttPublishDiscoveryStep(uint16_t step)']:
            code += function(signature) + '\n'
        code += r'''
int main() {
    static_assert(mqtt_discovery::entityCount == 32, "Entity set changed");
    for (uint16_t step = 0; step < DiscoveryProgress::total; ++step) {
        size_t before = mqttClient.messages.size();
        assert(mqttPublishDiscoveryStep(step));
        assert(mqttClient.messages.size() == before + 1); // Never burst publish.
        const auto& msg = mqttClient.messages.back();
        assert(msg.retained);
        if (step < 32) {
            const auto& e = mqtt_discovery::entities[step];
            assert(msg.topic == "custom/ha/" + String(e.component) + "/esp32mic_000000000000/" + e.objectId + "/config");
            assert(msg.payload.empty());
        } else if (step < 34) {
            assert(msg.topic == (step == 32 ? "esp32mic/esp32mic_000000000000/availability" : "esp32mic/DEVID/availability"));
            assert(msg.payload.empty());
        } else {
            const auto& e = mqtt_discovery::entities[step - 34];
            assert(msg.topic == "custom/ha/" + String(e.component) + "/DEVID/" + e.objectId + "/config");
            std::cout << e.component << '\t' << e.objectId << '\t' << msg.payload << '\n';
        }
    }
    size_t before = mqttClient.messages.size();
    assert(!mqttPublishDiscoveryStep(66));
    assert(!mqttPublishDiscoveryStep(UINT16_MAX));
    assert(mqttClient.messages.size() == before);
    mqttClient.succeed = false;
    assert(!mqttPublishDiscoveryStep(34)); // Existing retry logic sees failure.
    mqttClient.succeed = true;
    mqttDeviceId = "esp32mic_000000000000";
    before = mqttClient.messages.size();
    for (uint16_t i = 0; i < 32; ++i) assert(mqttPublishDiscoveryStep(i));
    assert(mqttClient.messages.size() == before); // Never erase our own entity configs.
    mqttDeviceId = "DEVID";
    availability = "esp32mic/DEVID/availability";
    assert(mqttPublishDiscoveryStep(33));
    assert(mqttClient.messages.size() == before); // Never erase current availability.
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
        expected = (ROOT / 'tests/fixtures/mqtt_discovery_v126.tsv').read_text()
        self.assertEqual(result.stdout, expected)
        # Verify the golden data is valid JSON, with stable unique IDs for all entities.
        ids = set()
        for line in result.stdout.splitlines():
            _, _, payload = line.split('\t')
            item = json.loads(payload)
            self.assertNotIn(item['uniq_id'], ids)
            ids.add(item['uniq_id'])
        self.assertEqual(len(ids), 32)
