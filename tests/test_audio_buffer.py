"""Exercise the firmware consumer against byte-buffer batching and wraparound.

ESP-IDF byte buffers merge adjacent writes. The fake implements that documented
contract; the consumer and explicit flush functions are compiled from the sketch.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def firmware_function(source, name):
    start = source.index("void " + name + "() {")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
struct ByteRing {
    std::vector<uint8_t> data;
    size_t head, available = 0, borrowed = 0;
    explicit ByteRing(size_t capacity, size_t start = 0): data(capacity), head(start) {}
    void put(const std::vector<int16_t>& samples) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
        size_t count = samples.size() * sizeof(int16_t);
        assert(count <= data.size() - available);
        for (size_t i = 0; i < count; ++i)
            data[(head + available + i) % data.size()] = bytes[i];
        available += count;
    }
    void* receive(size_t* count, size_t limit) {
        assert(borrowed == 0);
        if (!available) return nullptr;
        borrowed = std::min({available, data.size() - head, limit});
        *count = borrowed;
        return data.data() + head;
    }
    void giveBack(void* item) {
        assert(item == data.data() + head && borrowed);
        head = (head + borrowed) % data.size();
        available -= borrowed;
        borrowed = 0;
    }
};
using RingbufHandle_t = ByteRing*;
RingbufHandle_t audioRingBuffer;
void* xRingbufferReceive(RingbufHandle_t r, size_t* n, int) {
    return r->receive(n, r->data.size());
}
[[maybe_unused]] void* xRingbufferReceiveUpTo(RingbufHandle_t r, size_t* n, int, size_t maxBytes) {
    return r->receive(n, maxBytes);
}
void vRingbufferReturnItem(RingbufHandle_t r, void* p) { r->giveBack(p); }
uint32_t audioRingBufferFlushCount = 0, audioPacketsSent = 0;
bool audioRingBufferFlushPending = false;
int currentBufferSize;
uint32_t audioChunkSequence = 0;
uint32_t countedAudioChunk = 0, countedProfileChunk[2] = {0, 0};
unsigned long lastRtpPacketMs = 0;
unsigned long millis() { return 100; }
int16_t networkBuffer[4096];
int16_t* i2s_16bit_network_buffer = networkBuffer;
constexpr int MAX_CLIENTS = 2;
struct FakeWriter { bool busy = false; bool pending() const { return busy; } };
struct ClientSession {
    bool streaming; uint8_t profileIndex; FakeWriter tcpWriter;
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0, packetsSent = 0;
    unsigned long lastRtpPacketMs = 0;
};
ClientSession clients[MAX_CLIENTS];
struct StreamStats { uint32_t packetsSent; } streamStats[2];
std::vector<int16_t> delivered[2];
bool requestFlush = false;
void recordRtpSent(ClientSession&, uint16_t, uint32_t);
void sendRTPPacket(ClientSession& client, const int16_t* samples, int count, uint32_t chunk) {
    assert(count <= currentBufferSize);
    for (int i = 0; i < count; ++i) {
        uint16_t network = static_cast<uint16_t>(samples[i]);
        delivered[client.profileIndex].push_back(static_cast<int16_t>((network >> 8) | (network << 8)));
    }
    if (requestFlush) audioRingBufferFlushPending = true;
    recordRtpSent(client, uint16_t(count), chunk);
}
'''

MAIN = r'''
void runCase(int blockSize, int blocks, size_t start) {
    ByteRing ring(65536, start);
    audioRingBuffer = &ring;
    currentBufferSize = blockSize;
    audioPacketsSent = 0;
    audioRingBufferFlushPending = false;
    requestFlush = false;
    clients[0] = {true, 0, {false}}; clients[1] = {true, 1, {false}};
    delivered[0].clear(); delivered[1].clear();
    std::vector<int16_t> expected;
    for (int b = 0; b < blocks; ++b) {
        std::vector<int16_t> samples;
        for (int i = 0; i < blockSize; ++i) samples.push_back(static_cast<int16_t>(b * blockSize + i));
        ring.put(samples);
        expected.insert(expected.end(), samples.begin(), samples.end());
    }
    streamAudio();
    if (blocks > 4) assert(ring.available > 0); // Bounded work per main-loop pass.
    while (ring.available) streamAudio();
    assert(delivered[0] == expected);
    assert(delivered[1] == expected);
    assert(ring.borrowed == 0);
}
int main() {
    runCase(512, 2, 0); // Regression: two writes must not lose the second block.
    for (int block : {32, 512, 1024, 2048, 4096}) {
        runCase(block, 3, 0);
        runCase(block, 3, 65536 - block * sizeof(int16_t));
    }
    runCase(512, 7, 0);
    // Explicit recovery flushing remains allowed and frees all queued bytes.
    ByteRing ring(8192);
    audioRingBuffer = &ring; currentBufferSize = 512;
    delivered[0].clear(); delivered[1].clear();
    ring.put(std::vector<int16_t>(1536, 42));
    requestFlush = true; streamAudio();
    assert(ring.available == 0 && ring.borrowed == 0);
    assert(delivered[0].size() == 512 && delivered[1].size() == 512);
    // A blocked client must not prevent a healthy client from consuming audio.
    requestFlush = false;
    delivered[0].clear(); delivered[1].clear();
    clients[0].tcpWriter.busy = true;
    ring.put(std::vector<int16_t>(1024, 7));
    streamAudio();
    assert(delivered[0].empty() && delivered[1].size() == 1024);
    // With every client blocked, leave the shared queue intact for recovery.
    clients[1].tcpWriter.busy = true;
    ring.put(std::vector<int16_t>(512, 8));
    streamAudio();
    assert(ring.available == 1024 && ring.borrowed == 0);
    // Copies to two clients of the same profile count once, including a late
    // completion of a frame already delivered by the healthy client.
    clients[0].tcpWriter.busy = clients[1].tcpWriter.busy = false;
    clients[0].profileIndex = clients[1].profileIndex = 0;
    uint32_t countBefore = streamStats[0].packetsSent;
    uint32_t audioBefore = audioPacketsSent;
    streamAudio();
    assert(streamStats[0].packetsSent == countBefore + 1);
    assert(audioPacketsSent == audioBefore + 1);
    recordRtpSent(clients[0], 512, audioChunkSequence);
    assert(streamStats[0].packetsSent == countBefore + 1);
    assert(audioPacketsSent == audioBefore + 1);
    std::cout << "Audio batching, wraparound, bounded work and explicit recovery flush passed\n";
}
'''


class AudioBufferTests(unittest.TestCase):
    def test_reserve_capacity_keeps_large_packet_policy(self):
        source = (ROOT / "esp32-birdnet-mic/esp32-birdnet-mic.ino").read_text()
        start = source.index("static size_t computeAudioRingBufferCapacity()")
        end = source.index("void flushAudioRingBuffer()", start)
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "capacity.cpp"
            binary = pathlib.Path(temp) / "capacity"
            cpp.write_text("#include <cstddef>\n#include <cstdint>\n#include <cassert>\n"
                           "int currentBufferSize;\n" + source[start:end] + r'''
int main() {
    int sizes[] = {32, 256, 512, 1024, 2048};
    for (int size: sizes) {
        currentBufferSize = size;
        assert(computeAudioRingBufferCapacity() == 65536);
    }
    currentBufferSize = 4096;
    assert(computeAudioRingBufferCapacity() == 32768);
    currentBufferSize = 8192;
    assert(computeAudioRingBufferCapacity() == 49152);
    currentBufferSize = 16384;
    assert(computeAudioRingBufferCapacity() == 98304);
}
''')
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_firmware_preserves_queued_samples(self):
        source = (ROOT / "esp32-birdnet-mic/esp32-birdnet-mic.ino").read_text()
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "audio_test.cpp"
            binary = pathlib.Path(temp) / "audio_test"
            start = source.index('static void recordRtpSent(')
            end = source.index('static void stopStreamOnWriteFailure', start)
            counter = source[start:end].replace('static void recordRtpSent(', 'void recordRtpSent(', 1)
            cpp.write_text(HARNESS + counter + firmware_function(source, "flushAudioRingBuffer")
                           + firmware_function(source, "streamAudio") + MAIN)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
