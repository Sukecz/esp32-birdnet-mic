#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace recording {
// PCM16 mono, little endian. Checkpoint discipline follows the separate
// esp32-audio-recorder prototype: short writes poison the current .part file.
inline void wavHeader(uint8_t out[44], uint32_t rate, uint32_t bytes) {
    memset(out, 0, 44);
    auto le32 = [out](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out[at+i] = uint8_t(value >> (8*i));
    };
    memcpy(out, "RIFF", 4); le32(4, 36 + bytes);
    memcpy(out+8, "WAVEfmt ", 8); le32(16, 16);
    out[20] = 1; out[22] = 1; le32(24, rate); le32(28, rate*2);
    out[32] = 2; out[34] = 16; memcpy(out+36, "data", 4); le32(40, bytes);
}
template<class Sink> class WavWriter {
public:
    explicit WavWriter(Sink& sink) : sink_(sink) {}
    bool begin(uint32_t rate) {
        if (rate < 8000 || rate > 96000) return false;
        rate_ = rate; bytes_ = 0; failed_ = false;
        uint8_t header[44]; wavHeader(header, rate, 0);
        return put(header, 44);
    }
    bool append(const uint8_t* data, size_t bytes) {
        if (bytes % 2 || bytes > 0xffffffd0UL - bytes_ || failed_) return false;
        if (!put(data, bytes)) return false;
        bytes_ += bytes;
        return true;
    }
    bool checkpoint() {
        if (failed_) return false;
        uint8_t header[44]; wavHeader(header, rate_, bytes_);
        if (!sink_.seek(0) || !put(header, 44) || !sink_.sync() || !sink_.seek(44+bytes_)) {
            failed_ = true; return false;
        }
        return true;
    }
    uint32_t bytes() const { return bytes_; }
private:
    bool put(const uint8_t* p, size_t n) {
        if (failed_ || sink_.write(p, n) != n) { failed_ = true; return false; }
        return true;
    }
    Sink& sink_;
    uint32_t rate_ = 48000, bytes_ = 0;
    bool failed_ = false;
};
}
