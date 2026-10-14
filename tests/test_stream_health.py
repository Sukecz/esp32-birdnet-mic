"""Compile production health and MQTT paths against regression scenarios."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()


def function(signature):
    start = SOURCE.index(signature)
    brace = SOURCE.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


class StreamHealthTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_recent_rate_recovers_expires_resets_and_wraps(self):
        self.compile_run(r'''
#include "StreamHealth.h"
#include <cassert>
int main() {
    RecentPacketRate rate;
    uint32_t packets = 0;
    rate.reset(0, packets);
    assert(rate.rate(500, 46) == 0);
    for (uint32_t t = 1000; t <= 120000; t += 1000) {
        if (t > 100000) packets += 93;
        rate.update(t, packets);
    }
    // Old session average is only15 pkt/s; recovered recent output is93.
    assert(packets * 1000 / 120000 < 66);
    assert(rate.rate(120000, packets) == 93);
    for (uint32_t t = 121000; t <= 130000; t += 1000) rate.update(t, packets);
    assert(rate.rate(130000, packets) == 0);
    assert(rate.rate(150000, packets) == 0); // No stale samples.
    rate.reset(150000, 0);
    rate.update(151000, 93);
    assert(rate.rate(151000, 93) == 93);
    uint32_t base = UINT32_MAX - 1500, first = UINT32_MAX - 50;
    rate.reset(base, first);
    for (uint32_t i = 1; i <= 12; ++i) rate.update(base + i * 1000, first + i * 93);
    assert(rate.rate(base + 12000, first + 12 * 93) == 93);
    assert(audioProducerNeedsRecovery(40000, 37000, false, 0));
    assert(!audioProducerNeedsRecovery(40000, 39500, false, 0));
    assert(!audioProducerNeedsRecovery(40000, 37000, true, 0));
    assert(!audioProducerNeedsRecovery(40000, 37000, false, 20000));
    assert(audioProducerNeedsRecovery(1000, UINT32_MAX - 2000, false, UINT32_MAX - 40000));
}
''')

    def test_actual_recovery_ignores_network_rate_and_recovers_failed_capture(self):
        self.compile_run(r'''
#include "StreamHealth.h"
#include <cassert>
#include <string>
using String = std::string;
uint32_t nowMs = 60000, currentRate = 93, minFreeHeap = UINT32_MAX;
uint32_t maxPacketRate = 0, minPacketRate = UINT32_MAX, minAcceptableRate = 66;
unsigned long lastStatsReset = 0, lastI2SReset = 0;
volatile uint32_t audioProducerLastReadMs = 59990;
volatile bool audioProducerStopRequested = false;
bool isStreaming = true, autoRecoveryEnabled = true;
int restarts = 0;
struct { uint32_t getFreeHeap() { return 200000; } } ESP;
uint32_t millis() { return nowMs; }
uint32_t getCurrentPacketRate(uint8_t) { return currentRate; }
void simplePrintln(const String&) {}
bool restartI2S() { ++restarts; return true; }
''' + function('void checkPerformance()') + function('void checkAudioProducerHealth()') + r'''
int main() {
    checkPerformance(); checkAudioProducerHealth();
    assert(restarts == 0 && minPacketRate == 93);
    currentRate = 0; // Network blocked, successful I2S reads continue.
    checkPerformance(); checkAudioProducerHealth();
    assert(restarts == 0);
    audioProducerLastReadMs = 55000; // Actual stalled capture, persistent2s.
    checkAudioProducerHealth();
    assert(restarts == 1 && lastI2SReset == nowMs);
    nowMs += 1000; checkAudioProducerHealth();
    assert(restarts == 1); // Cooldown prevents reset loop.
    nowMs += 30000; autoRecoveryEnabled = false; checkAudioProducerHealth();
    assert(restarts == 1);
    autoRecoveryEnabled = true; audioProducerStopRequested = true; checkAudioProducerHealth();
    assert(restarts == 1);
    audioProducerStopRequested = false; isStreaming = false; checkAudioProducerHealth();
    assert(restarts == 1);
}
''')

    def test_forced_mqtt_updates_are_deferred_coalesced_and_retried(self):
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <string>
using String = std::string;
uint32_t nowMs = 1000;
uint32_t millis() { return nowMs; }
unsigned long lastMqttPublishMs = 0;
uint16_t mqttPublishIntervalSec = 60;
bool mqttStatePublishPending = false;
String mqttLastError = "ok";
struct { bool failed = false; } mqttDiscoveryProgress;
String mqttStateTopic() { return "state"; }
String mqttBuildStateJson() { return "latest"; }
struct {
    bool alive = true, succeed = true;
    int publishes = 0;
    bool connected() { return alive; }
    bool publish(const char*, const char*, bool) { ++publishes; return succeed; }
} mqttClient;
''' + function('static bool mqttPublishState(bool force)') + r'''
int main() {
    assert(mqttPublishState(true)); assert(mqttPublishState(true));
    assert(mqttClient.publishes == 0 && mqttStatePublishPending);
    assert(mqttPublishState(false));
    assert(mqttClient.publishes == 1 && !mqttStatePublishPending);
    assert(mqttPublishState(false) && mqttClient.publishes == 1);
    mqttPublishState(true); mqttClient.succeed = false;
    assert(!mqttPublishState(false) && mqttStatePublishPending);
    mqttClient.alive = false;
    assert(mqttPublishState(true) && !mqttPublishState(false));
    mqttClient.alive = true; mqttClient.succeed = true;
    assert(mqttPublishState(false) && !mqttStatePublishPending);
    nowMs += 60000;
    int before = mqttClient.publishes;
    assert(mqttPublishState(false) && mqttClient.publishes == before + 1);
}
''')

    def test_actual_pending_writer_flushes_only_overflow_without_healthy_peer(self):
        self.compile_run(r'''
#include "StreamHealth.h"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <sys/socket.h>
class CooperativeTcpWriter {
public:
    enum Result { COMPLETE, FAILED, TIMED_OUT, PENDING };
    bool blocked = true;
    Result result = COMPLETE;
    bool pending() const { return true; }
    bool stalled() const { return blocked; }
    template<class Sender> Result poll(uint32_t, Sender) { return result; }
    void clear() {}
};
struct ClientSession {
    struct { bool connected() { return true; } int fd() { return -1; } } client;
    CooperativeTcpWriter tcpWriter;
    bool streaming = true, tcpFrameIsRtp = true;
    uint16_t tcpFrameSamples = 512;
    uint32_t tcpFrameChunk = 1, tcpFrameDropsAtQueue = 10;
};
uint32_t nowMs = 300, rtspWriteStallCount = 0, rtspWriteTimeoutCount = 0;
uint32_t audioRingBufferDropCount = 10;
bool peer = false, audioRingBufferFlushPending = false;
int sent = 0, failed = 0;
uint32_t millis() { return nowMs; }
bool anotherSessionStreaming(const ClientSession&) { return peer; }
void recordRtpSent(ClientSession&, uint16_t, uint32_t) { ++sent; }
void stopStreamOnWriteFailure(ClientSession&, const char*) { ++failed; }
''' + function('static void servicePendingTcp(ClientSession &session)') + r'''
int main() {
    ClientSession session;
    servicePendingTcp(session);
    assert(sent == 1 && !audioRingBufferFlushPending); // Old250ms policy discarded this.
    audioRingBufferDropCount = 11;
    servicePendingTcp(session);
    assert(audioRingBufferFlushPending); // Overflow makes prefix stale.
    audioRingBufferFlushPending = false; peer = true;
    servicePendingTcp(session);
    assert(!audioRingBufferFlushPending);
    peer = false; session.tcpWriter.result = CooperativeTcpWriter::PENDING;
    servicePendingTcp(session);
    assert(!audioRingBufferFlushPending);
    session.tcpWriter.result = CooperativeTcpWriter::TIMED_OUT;
    servicePendingTcp(session);
    assert(failed == 1 && rtspWriteTimeoutCount == 1);
}
''')

    def test_real_mqtt_socket_partial_write_closes_instead_of_blocking(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / 'mqtt'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'tests/stubs'),
                            str(ROOT / 'tests/discovery_client_test.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
