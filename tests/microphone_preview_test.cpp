#include "MicrophoneSignal.h"
#include "PreviewBuffer.h"
#include <assert.h>
#include <thread>
#include <vector>

static std::atomic<uint32_t> outstanding{0};

int main() {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    static_assert(PreviewBuffer::capacity == 4096, "C5 must free 12 KiB for Wi-Fi/capture");
#else
    static_assert(PreviewBuffer::capacity == 16384, "Other boards keep the full preview queue");
#endif
    MicrophoneSignal health;
    assert(health.state(0) == MicrophoneSignal::Unavailable);
    health.observe(0, 0, 0);
    assert(health.state(0) == MicrophoneSignal::Checking);
    health.observe(0, 0, 19999);
    assert(health.state(19999) == MicrophoneSignal::Checking);
    health.observe(0, 0, 20000);
    assert(health.state(20000) == MicrophoneSignal::Flatline);
    assert(health.flatSeconds(20000) == 20);
    assert(health.state(22000) == MicrophoneSignal::Unavailable); // Missing data is distinct.
    assert(health.state(20000, false) == MicrophoneSignal::Unavailable);
    health.observe(-1, 1, 22001); // Quiet microphone noise is sufficient.
    assert(health.state(22001) == MicrophoneSignal::Healthy);
    assert(health.flatSeconds(22001) == 0);
    for (int32_t value : {int32_t(123), INT32_MAX, INT32_MIN}) {
        health.reset();
        health.observe(value, value, 100);
        health.observe(value, value, 20100);
        assert(health.state(20100) == MicrophoneSignal::Flatline);
    }
    health.reset(); health.observe(7, 7, UINT32_MAX - 9999);
    health.observe(7, 7, 10000); // millis rollover.
    assert(health.state(10000) == MicrophoneSignal::Flatline);
    health.reset(); health.observe(3, 3, 0); health.observe(4, 4, 21000);
    assert(health.state(21000) == MicrophoneSignal::Healthy); // Change between blocks.

    PreviewBuffer preview;
    std::vector<uint8_t> in(PreviewBuffer::capacity), out(in.size());
    for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i);
    preview.push(in.data(), in.size());
    assert(preview.pop(out.data(), out.size()) == 0); // No listener: no queued audio.
    assert(preview.begin()); preview.push(in.data(), in.size());
    assert(preview.pop(out.data(), 1000) == 1000);
    assert(out[999] == in[999]);
    assert(preview.pop(out.data() + 1000, out.size()) == out.size() - 1000);
    assert(out == in);
    for (int i = 0; i < 100; ++i) {
        preview.push(in.data(), 1234); // Repeated wrap in the physical ring.
        assert(preview.pop(out.data(), 1234) == 1234);
        assert(memcmp(in.data(), out.data(), 1234) == 0);
    }
    preview.push(in.data(), in.size()); preview.push(in.data(), 2);
    assert(preview.overflowed()); // Bounded memory, never overwrite unread samples.
    assert(preview.takeDroppedBytes() == 2);
    assert(!preview.overflowed());
    // Reporting an overflow must preserve the full queue, not throw it away.
    assert(preview.pop(out.data(), out.size()) == in.size());
    assert(out == in);
    preview.push(in.data(), in.size());
    for(int i=0;i<3;++i) preview.push(in.data(),2);
    assert(preview.takeDroppedBytes() == 6);
    assert(preview.takeDroppedBytes() == 0);
    std::atomic<bool> rejectedDone{false};
    std::thread rejecting([&]{
        for(int i=0;i<100000;++i) preview.push(in.data(),2);
        rejectedDone.store(true);
    });
    uint32_t rejectedTotal=0;
    while(!rejectedDone.load()) rejectedTotal+=preview.takeDroppedBytes();
    rejecting.join();rejectedTotal+=preview.takeDroppedBytes();
    assert(rejectedTotal==200000); // No lost overflow reports across tasks.
    assert(preview.pop(out.data(),out.size())==in.size()&&out==in);
    preview.end(); assert(!preview.active());
    preview.begin(); assert(!preview.overflowed());
    assert(preview.pop(out.data(), out.size()) == 0); // Reconnect discards backlog.

    // Both I2S-converted PCM and HW/SW PDM-converted PCM use exactly this queue.
    // Stress publication/acquisition across tasks with odd-sized consumer reads.
    constexpr uint32_t count = 200000;
    std::thread writer([&] {
        for (uint32_t i = 0; i < count; ++i) {
            // Test uses a handshake, keeping queue space guaranteed while
            // exercising simultaneous release/acquire of actual PCM bytes.
            while (outstanding.load() != 0) std::this_thread::yield();
            preview.push(&i, sizeof(i)); outstanding.store(1);
        }
    });
    for (uint32_t i = 0; i < count; ++i) {
        while (outstanding.load() == 0) std::this_thread::yield();
        uint32_t value = 0;
        assert(preview.pop(&value, 3) == 3);
        assert(preview.pop(reinterpret_cast<uint8_t*>(&value) + 3, 1) == 1);
        assert(value == i); outstanding.store(0);
    }
    writer.join(); assert(!preview.overflowed());
}
