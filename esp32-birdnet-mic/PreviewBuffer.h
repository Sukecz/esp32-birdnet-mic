#pragma once
#if defined(__has_include)
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#endif
#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "AudioMemory.h"

// Independent, bounded PCM queue. No capture-side allocation, waiting or socket
// calls. Full queues reject only the incoming block, preserving unread PCM;
// capture and SD recording never wait for the preview connection.
class PreviewBuffer {
public:
#if defined(CONFIG_IDF_TARGET_ESP32C5) || AUDIO_RING_IN_PSRAM
    // Free 12 KiB of internal RAM for capture and Wi-Fi/TCP on C5.
    // At 48 kHz mono int16 this holds about 43 ms for one browser listener.
    static constexpr size_t capacity = 4096;
#else
    static constexpr size_t capacity = 16384;
#endif
    // Optional larger queue (S3/C5 with PSRAM). Call once before the first
    // begin(); the size must be a power of two so offsets survive uint32 wrap.
    bool useStorage(uint8_t* buffer, size_t bytes) {
        if (!buffer || bytes <= capacity || (bytes & (bytes - 1)) || active() || everStarted || buf != storage) return false;
        buf = buffer;
        size = bytes;
        return true;
    }
    size_t queueBytes() const { return size; }
    bool begin() {
        everStarted = true;
        // Only the consumer discards backlog. Monotonic offsets are never reset
        // while capture could still be publishing a block from the last session.
        tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
        droppedBytes.store(0);
        enabled.store(true);
        return true;
    }
    void end() { enabled.store(false); }
    bool active() const { return enabled.load(); }
    bool overflowed() const { return droppedBytes.load(std::memory_order_relaxed) != 0; }
    // Exchange only diagnostics, never the consumer position. Concurrent rejected
    // blocks are counted in this exchange or the next, without clearing a flag race.
    uint32_t takeDroppedBytes() { return droppedBytes.exchange(0, std::memory_order_relaxed); }
    void push(const void* data, size_t bytes) {
        if (!active()) return;
        uint32_t write = head.load(std::memory_order_relaxed);
        uint32_t read = tail.load(std::memory_order_acquire);
        if (bytes > size - uint32_t(write - read)) droppedBytes.fetch_add(bytes, std::memory_order_relaxed);
        else {
            size_t offset = write % size;
            size_t first = bytes < size - offset ? bytes : size - offset;
            memcpy(buf + offset, data, first);
            memcpy(buf, static_cast<const uint8_t*>(data) + first, bytes - first);
            head.store(write + bytes, std::memory_order_release);
        }
    }
    size_t pop(void* data, size_t bytes) {
        uint32_t read = tail.load(std::memory_order_relaxed);
        uint32_t used = head.load(std::memory_order_acquire) - read;
        if (bytes > used) bytes = used;
        size_t offset = read % size;
        size_t first = bytes < size - offset ? bytes : size - offset;
        memcpy(data, buf + offset, first);
        memcpy(static_cast<uint8_t*>(data) + first, buf, bytes - first);
        tail.store(read + bytes, std::memory_order_release);
        return bytes;
    }
private:
    uint8_t storage[capacity];
    bool everStarted = false;
    uint8_t* buf = storage;
    size_t size = capacity;
    std::atomic<uint32_t> head{0}, tail{0};
    std::atomic<uint32_t> droppedBytes{0};
    std::atomic<bool> enabled{false};
};
