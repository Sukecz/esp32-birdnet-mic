#pragma once
#include <stdint.h>

// Keep recent counter snapshots; a recovered stream must not inherit an old
// low lifetime average. Counter and millis differences also handle wraparound.
class RecentPacketRate {
public:
    void reset(uint32_t now, uint32_t packets) {
        count = 1;
        next = 1;
        samples[0] = {now, packets};
        lastSample = now;
    }
    void update(uint32_t now, uint32_t packets) {
        if (!count) { reset(now, packets); return; }
        if (uint32_t(now - lastSample) < 1000) return;
        samples[next] = {now, packets};
        next = (next + 1) % capacity;
        if (count < capacity) ++count;
        lastSample = now;
    }
    uint32_t rate(uint32_t now, uint32_t packets) const {
        uint32_t oldestAge = 0, oldestPackets = packets;
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t age = now - samples[i].at;
            if (age <= 10000 && age > oldestAge) {
                oldestAge = age;
                oldestPackets = samples[i].packets;
            }
        }
        if (oldestAge < 1000) return 0;
        return uint32_t(uint64_t(uint32_t(packets - oldestPackets)) * 1000 / oldestAge);
    }
private:
    struct Sample { uint32_t at, packets; };
    static constexpr uint8_t capacity = 11;
    Sample samples[capacity] = {};
    uint8_t count = 0, next = 0;
    uint32_t lastSample = 0;
};

inline bool audioProducerNeedsRecovery(uint32_t now, uint32_t lastRead,
                                      bool stopping, uint32_t lastRecovery) {
    // No output-rate condition: backpressure does not make I2S unhealthy.
    return !stopping && uint32_t(now - lastRead) >= 2000 &&
           uint32_t(now - lastRecovery) >= 30000;
}

inline bool shouldFlushRecoveredBacklog(bool stalled, bool streaming,
                                       bool anotherStreaming,
                                       uint32_t dropsAtQueue, uint32_t dropsNow) {
    // Preserve valid audio through short stalls. After actual overflow, discard
    // the stale prefix once; never clear audio required by a healthy peer.
    return stalled && streaming && !anotherStreaming && dropsNow != dropsAtQueue;
}
