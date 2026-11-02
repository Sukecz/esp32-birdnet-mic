// Host test for MicAutoDetect.h classifiers using synthetic captures.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "MicAutoDetect.h"

static uint32_t seed = 12345;
static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed; }
static double gauss() { double s = 0; for (int i = 0; i < 12; ++i) s += (rnd() >> 8) / 16777216.0; return s - 6.0; }

static const size_t N = 2048;

// 24-bit mic (ICS-43434/INMP441) in Philips timing: data in bits 31..8.
static std::vector<int32_t> ics(double rms24) {
    std::vector<int32_t> w(N);
    for (auto& v : w) v = (int32_t)((uint32_t)(int32_t)lround(gauss() * rms24) << 8);
    return w;
}

// SPH0645 (18-bit, MSB timing) read in Philips timing: MSB lost, bits 16..0
// land in 31..15, then the idle bus (held at the last bit or pulled low).
static std::vector<int32_t> sph(double rms18, bool hold) {
    std::vector<int32_t> w(N);
    for (auto& v : w) {
        int32_t s = (int32_t)lround(gauss() * rms18);
        uint32_t u = ((uint32_t)s & 0x1FFFFu) << 15;
        if (hold && (s & 1)) u |= 0x7FFFu;
        v = (int32_t)u;
    }
    return w;
}

int main() {
    using namespace micdetect;
    // Quiet and moderately loud 24-bit mics.
    for (double rms : {16.0, 4096.0, 200000.0}) {
        auto w = ics(rms);
        assert(classifyI2s(w.data(), N) == Philips);
    }
    // SPH0645, idle bus pulled low or holding the last bit.
    for (bool hold : {false, true}) for (double rms : {8.0, 1024.0, 3000.0}) {
        auto w = sph(rms, hold);
        assert(classifyI2s(w.data(), N) == Msb);
    }
    // No mic: stuck low or high.
    std::vector<int32_t> z(N, 0), o(N, -1);
    assert(classifyI2s(z.data(), N) == None);
    assert(classifyI2s(o.data(), N) == None);
    // PDM mic clocked by the I2S probe: random bits, not I2S.
    std::vector<int32_t> r(N);
    for (auto& v : r) v = (int32_t)rnd();
    assert(classifyI2s(r.data(), N) == None);

    // Raw PDM: first-order sigma-delta of a small sine plus noise.
    std::vector<uint16_t> pdm(N);
    double integ = 0; int fb = 0; size_t k = 0;
    for (auto& v : pdm) {
        uint16_t word = 0;
        for (int b = 0; b < 16; ++b, ++k) {
            double x = 0.05 * sin(k * 2 * M_PI / 1000.0) + 0.01 * gauss();
            integ += x - (fb ? 1.0 : -1.0);
            fb = integ > 0;
            word = (uint16_t)((word << 1) | fb);
        }
        v = word;
    }
    assert(pdmRawValid(pdm.data(), N));
    std::vector<uint16_t> pz(N, 0), po(N, 0xFFFF);
    assert(!pdmRawValid(pz.data(), N));
    assert(!pdmRawValid(po.data(), N));

    // Hardware PDM->PCM (S3): any varying signal is valid (quiet, loud or with
    // a DC offset); a stuck or railed line is not.
    std::vector<int16_t> pcm(N), flat(N, -32768), junk(N), dc(N), rail(N);
    for (auto& v : pcm) v = (int16_t)lround(gauss() * 3.0);
    for (auto& v : junk) v = (int16_t)(rnd() >> 16);
    for (auto& v : dc) v = (int16_t)(20000 + lround(gauss() * 500.0));
    for (size_t i = 0; i < N; ++i) rail[i] = (i & 1) ? 32767 : -32768;
    assert(pdmPcmValid(pcm.data(), N));
    std::vector<int16_t> quiet(N); // a very quiet mic: mostly 0 with a few LSB steps
    for (auto& v : quiet) v = (int16_t)lround(gauss() * 0.4);
    assert(pdmPcmValid(quiet.data(), N));
    assert(!pdmPcmValid(flat.data(), N));
    assert(pdmPcmValid(junk.data(), N));
    assert(pdmPcmValid(dc.data(), N));
    assert(!pdmPcmValid(rail.data(), N));
    assert(pcmStats(pcm.data(), N).rms <= 4);

    // Gain rule.
    assert(startingGain(true, 1.5f) == 24.0f);
    assert(startingGain(true, 12.0f) == 12.0f);
    assert(startingGain(false, 24.0f) == 1.5f);
    assert(startingGain(false, 3.0f) == 3.0f);
    std::puts("mic auto-detect OK");
    return 0;
}
