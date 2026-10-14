#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// One owned frame per client. RTSP replies and interleaved RTP use the same
// writer, so a partial frame can never be overwritten or interleaved.
class CooperativeTcpWriter {
public:
    enum Result { IDLE, PENDING, COMPLETE, FAILED, TIMED_OUT };
    CooperativeTcpWriter() = default;
    CooperativeTcpWriter(const CooperativeTcpWriter&) = delete;
    CooperativeTcpWriter& operator=(const CooperativeTcpWriter&) = delete;
    ~CooperativeTcpWriter() { release(); }

    bool queue(const uint8_t* data, size_t size, uint32_t now) {
        if (pending() || !size) return false;
        if (size > capacity_) {
            void* grown = realloc(data_, size);
            if (!grown) return false;
            data_ = static_cast<uint8_t*>(grown);
            capacity_ = size;
        }
        memcpy(data_, data, size);
        length_ = size;
        offset_ = 0;
        started_ = now;
        stalled_ = false;
        retryPending_ = false;
        return true;
    }

    // The callback must perform a non-blocking send and set errno on failure.
    // Bound syscall count even when partial writes make continuous progress.
    template <typename Send>
    Result poll(uint32_t now, Send sendBytes) {
        if (!pending()) return IDLE;
        if (uint32_t(now - started_) >= 10000UL) return TIMED_OUT;
        if (retryPending_ && uint32_t(now - attempted_) < 1) return PENDING;
        retryPending_ = false;
        attempted_ = now;
        for (uint8_t attempts = 0; attempts < 4; ++attempts) {
            errno = 0;
            int written = sendBytes(data_ + offset_, length_ - offset_);
            if (written > 0) {
                if (size_t(written) > length_ - offset_) return FAILED;
                offset_ += size_t(written);
                if (offset_ == length_) return COMPLETE;
                continue;
            }
            if (written == 0) return FAILED;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return FAILED;
            stalled_ = true;
            retryPending_ = true;
            return PENDING;
        }
        return PENDING;
    }

    bool pending() const { return length_ != 0; }
    bool stalled() const { return stalled_; }
    uint32_t age(uint32_t now) const { return uint32_t(now - started_); }
    void clear() { length_ = offset_ = 0; stalled_ = retryPending_ = false; }
    void release() { free(data_); data_ = nullptr; capacity_ = 0; clear(); }

private:
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0, length_ = 0, offset_ = 0;
    uint32_t started_ = 0, attempted_ = 0;
    bool stalled_ = false, retryPending_ = false;
};
