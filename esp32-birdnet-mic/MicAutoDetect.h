// Based on Dan Logue's November 2026 contribution.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <math.h>

// Microphone auto-detect: pure classifiers for short raw captures taken at
// boot (or when Auto is selected). Kept free of Arduino/IDF so the host tests
// can exercise them with synthetic data. See MIC_AUTO_DETECT_NOTES.md.
namespace micdetect {

enum Result : uint8_t { None = 0, Philips = 1, Msb = 2, Pdm = 3 };

// Starting gains used only when Auto finds a different microphone family.
static constexpr float GAIN_THRESHOLD = 8.0f; // between the 1.5 and 24 defaults
static constexpr float PDM_GAIN = 24.0f;
static constexpr float I2S_GAIN = 1.5f;

struct I2sStats {
    uint32_t words = 0, changes = 0, signWords = 0, lowUniform = 0;
};

inline I2sStats i2sStats(const int32_t* w, size_t n) {
    I2sStats s;
    s.words = (uint32_t)n;
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = w[i];
        if (i && v != w[i - 1]) s.changes++;
        // Quiet audio: the top bits only repeat the sign bit.
        const int32_t top = v >> 28;
        if (top == 0 || top == -1) s.signWords++;
        // Bits 14..8 read in Philips timing: active data LSBs for a 24-bit
        // mic (random), but the bus after the SPH0645's 18th bit (constant).
        const uint32_t low = ((uint32_t)v >> 8) & 0x7Fu;
        if (low == 0 || low == 0x7Fu) s.lowUniform++;
    }
    return s;
}

// Classifies one Philips-format capture (32-bit slots, left channel).
inline Result classifyI2s(const int32_t* w, size_t n) {
    if (n < 256) return None;
    const I2sStats s = i2sStats(w, n);
    if (s.changes < n / 16) return None;              // stuck line, no mic
    if (s.signWords * 10u < s.words * 7u) return None; // random bits (e.g. PDM)
    return s.lowUniform * 10u >= s.words * 6u ? Msb : Philips;
}

// Raw PDM bitstream words (software-PDM targets: C3/C5/C6).
struct PdmRawStats { uint32_t words = 0, ones = 0, uniform = 0, changes = 0; };
inline PdmRawStats pdmRawStats(const uint16_t* w, size_t n) {
    PdmRawStats s;
    s.words = (uint32_t)n;
    for (size_t i = 0; i < n; ++i) {
        uint16_t v = w[i];
        if (v == 0 || v == 0xFFFFu) s.uniform++;
        if (i && v != w[i - 1]) s.changes++;
        while (v) { s.ones += v & 1u; v >>= 1; }
    }
    return s;
}
inline bool pdmRawValid(const PdmRawStats& s) {
    const uint32_t bits = s.words * 16u;
    return s.words >= 256 && s.ones * 4u > bits && s.ones * 4u < bits * 3u && // 25..75% ones
           s.uniform * 5u < s.words && s.changes * 4u > s.words;
}
inline bool pdmRawValid(const uint16_t* w, size_t n) { return pdmRawValid(pdmRawStats(w, n)); }

// Hardware PDM->PCM output (S3). A stuck data line gives a constant (or
// railed) value; any microphone gives a varying signal. Level and DC offset
// are deliberately not judged: built-in mics differ a lot.
struct PcmStats { uint32_t words = 0, changes = 0, railed = 0; int32_t mean = 0; uint32_t rms = 0; };
inline PcmStats pcmStats(const int16_t* s, size_t n) {
    PcmStats st;
    st.words = (uint32_t)n;
    if (!n) return st;
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += s[i];
        if (i && s[i] != s[i - 1]) st.changes++;
        if (s[i] >= 32767 || s[i] <= -32767) st.railed++;
    }
    st.mean = (int32_t)(sum / (int64_t)n);
    uint64_t var = 0;
    for (size_t i = 0; i < n; ++i) {
        const int64_t d = (int64_t)s[i] - st.mean;
        var += (uint64_t)(d * d);
    }
    var /= n;
    st.rms = (uint32_t)sqrt((double)var);
    return st;
}
inline bool pdmPcmValid(const PcmStats& s) {
    return s.words >= 256 && s.changes * 32u > s.words && s.railed * 2u < s.words;
}
inline bool pdmPcmValid(const int16_t* s, size_t n) { return pdmPcmValid(pcmStats(s, n)); }

inline const char* name(Result r) {
    return r == Philips ? "ICS-43434/INMP441 (Philips I2S)" :
        r == Msb ? "SPH0645 (MSB I2S)" : r == Pdm ? "PDM" : "none";
}

// Gain rule requested by Dan: when Auto changes the microphone family,
// move a gain left over from the other family to this family's default.
inline float startingGain(bool pdm, float gain) {
    if (pdm && gain < GAIN_THRESHOLD) return PDM_GAIN;
    if (!pdm && gain > GAIN_THRESHOLD) return I2S_GAIN;
    return gain;
}

} // namespace micdetect
