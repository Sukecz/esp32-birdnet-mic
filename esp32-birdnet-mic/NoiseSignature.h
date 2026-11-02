// Based on Dan Logue's November 2026 contribution.
#pragma once
// Adaptive noise-signature engine (experimental).
//
// Integer-only spectral analysis of the int16 PCM stream after the existing
// high-pass/gain stage:
//
//   sqrt-Hann window -> 512-point real FFT (int32, Q31 twiddles)
//   -> per-bin log2 power (Q8) -> minimum-statistics noise floor tracker
//   -> long-term noise "signature" (learned over minutes)
//   -> per-bin spectral-subtraction gain (lookup table) -> inverse FFT
//   -> sqrt-Hann synthesis + 50% overlap-add.
//
// The firmware runs it in place on the stream (when the filter is On); see NoiseShadow.h.
// process() (output discarded) remains for the host tests and noise_wav.
//
// Plain C++ with no Arduino dependencies; tests/noise_signature_test.cpp
// checks it on the host. No libm: twiddles and window are computed into engine
// RAM at init with constexpr series helpers; only the log2 table is in flash.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef NSIG_HOT
#define NSIG_HOT
#endif

namespace nsig {

static const int kN = 512;               // frame length (samples)
static const int kH = kN / 2;            // hop (50% overlap)
static const int kM = kN / 2;            // complex FFT size used for the real FFT
static const int kBins = kN / 2 + 1;     // 0 .. Nyquist
static const int kInFrac = 6;            // extra fraction bits on windowed input
static const int kTwShift = 31;          // twiddles and window in Q31
static const int kOutFrac = 5;           // fraction bits of the overlap-add accumulator
static const int kSub = 8;               // minimum-statistics subwindows
static const int kGainLut = 256;         // gain table: 1/16-bit (0.19 dB) steps, 0..16 bits SNR
static const int kSigVersion = 1;

// ---------------------------------------------------------------- math (no libm)

namespace math {
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;

constexpr double taylorSin(double x) {   // |x| <= pi/2
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k <= 14; k++) { t *= -x2 / (double)((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
constexpr double taylorCos(double x) {
    double x2 = x * x, t = 1.0, s = 1.0;
    for (int k = 1; k <= 14; k++) { t *= -x2 / (double)((2 * k - 1) * (2 * k)); s += t; }
    return s;
}
constexpr double sin0pi(double x) { return x > kPi / 2 ? taylorSin(kPi - x) : taylorSin(x); }   // 0..pi
constexpr double cos0pi(double x) { return x > kPi / 2 ? -taylorCos(kPi - x) : taylorCos(x); }
constexpr int64_t roundI(double v) { return (int64_t)(v < 0 ? v - 0.5 : v + 0.5); }

// natural log for v > 0: reduce to [1,2) then 2*atanh((m-1)/(m+1))
constexpr double ln(double v) {
    int e = 0;
    while (v >= 2.0) { v *= 0.5; e++; }
    while (v < 1.0) { v *= 2.0; e--; }
    double z = (v - 1.0) / (v + 1.0), z2 = z * z, t = z, s = 0.0;
    for (int k = 0; k < 40; k++) { s += t / (2 * k + 1); t *= z2; }
    return 2.0 * s + e * kLn2;
}
constexpr double log2d(double v) { return ln(v) / kLn2; }
inline double exp2d(double x) {         // 2^x
    int n = (int)x;
    if ((double)n > x) n--;             // floor
    double f = (x - n) * kLn2, t = 1.0, s = 1.0;
    for (int k = 1; k < 25; k++) { t *= f / k; s += t; }
    while (n > 0) { s *= 2.0; n--; }
    while (n < 0) { s *= 0.5; n++; }
    return s;
}
inline double dbToLin(double db) { return exp2d(db / 20.0 * 3.32192809488736234787); }
inline double sqrtd(double v) {
    if (v <= 0) return 0;
    double r = v > 1 ? v : 1.0;
    for (int i = 0; i < 60; i++) r = 0.5 * (r + v / r);
    return r;
}
}  // namespace math

// Q31 with 1.0 saturated to 0x7FFFFFFF.
constexpr int32_t q31(double v) {
    return v >= 1.0 ? 0x7FFFFFFF : (int32_t)math::roundI(v * 2147483648.0);
}

// Only the 128-byte log table lives in flash. The window and FFT twiddles
// are computed into the engine's RAM by init(), saving ~4 KB of flash.
struct Tables {
    uint8_t log2Frac[128];              // round(256*log2(1 + i/128))
    constexpr Tables() : log2Frac() {
        for (int i = 0; i < 128; i++)
            log2Frac[i] = (uint8_t)math::roundI(256.0 * math::log2d(1.0 + i / 128.0));
    }
};
static constexpr Tables kT{};

// Window sums, exact for the periodic sqrt-Hann w[n] = sin(pi*n/N):
// sum(w^2) = N/2 and sum(w) = cot(pi/(2N)).
constexpr double kWinPowerSum = kN / 2.0;
constexpr double kWinSum = math::cos0pi(math::kPi / (2.0 * kN)) / math::sin0pi(math::kPi / (2.0 * kN));

struct Params {
    float subwindowSec = 15.0f;   // min-stat subwindow; tracker window = 8 x this (2 min)
    float learnSec = 120.0f;      // signature counts as "learned" after this long
    float biasDb = 0.0f;          // extra trim on top of the automatic minimum-statistics bias
    float gainFloorDb = -12.0f;   // most a bin is ever attenuated
    float overSub = 1.0f;         // spectral subtraction strength
    float releaseDbPerSec = 60.0f;// how fast a bin's gain may fall (rises are instant)
};

struct Stats {
    uint32_t frames;              // frames processed since start
    uint64_t inEnergy, outEnergy; // sums of squares (caller resets)
};

struct Engine {
    // --- RAM copies of the constant tables (flash reads through the cache
    //     are slow on the C3/C6 while Wi-Fi is busy) ---
    int32_t twc[kN / 2], tws[kN / 2], win[kN];
    uint8_t rev[kM];                    // bit-reversal permutation

    // --- parameter-dependent tables ---
    int16_t gainLut[kGainLut];          // Q15 amplitude gain by SNR
    int16_t gainFloor;                  // Q15
    int16_t releaseQ15;                 // per-frame multiplier for falling gain
    uint16_t biasQ8;
    uint16_t smoothShift;
    uint32_t subwindowFrames, learnFrames;
    uint32_t sampleRate;

    // --- signal state ---
    int16_t in[kN];
    int inPos;                          // samples in the second half of in[]
    int32_t buf[2 * kM];                // complex work buffer
    int32_t spec[2 * kBins];            // one-sided spectrum
    int32_t ola[kN];                    // overlap-add accumulator (input units << kOutFrac)
    int16_t out[kH];                    // last emitted hop
    int16_t ref[kH];                    // input aligned with out[] (for energy comparison)
    int16_t fifo[3 * kH];               // in-place mode: emitted samples not yet written back
    int fifoHead, fifoCount;

    // --- noise tracking (all log2 power, Q8) ---
    uint16_t smooth[kBins];             // smoothed power (~40 ms)
    uint16_t level[kBins];              // instantaneous power (last frame)
    uint16_t curMin[kBins];             // minimum in the current subwindow
    uint16_t subMin[kSub][kBins];       // minima of completed subwindows
    uint16_t floorQ8[kBins];            // tracker floor = min over subwindows + bias
    uint16_t sig[kBins];                // long-term signature
    int16_t gainPrev[kBins];            // Q15
    uint32_t frameInSub;
    uint8_t subIdx, subFilled;
    bool trackerValid;
    bool sigValid;                      // sig[] holds a usable signature
    uint32_t sigFrames;                 // frames since the signature started
    uint32_t sigUpdates;
    uint16_t holdFrames;                // skip minimum tracking while > 0 (settling)

    Stats stats;
};

// ---------------------------------------------------------------- helpers

static inline int16_t sat16(int32_t v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}
static inline int clz64(uint64_t v) {
#if defined(__GNUC__)
    return __builtin_clzll(v);
#else
    int n = 0; while (!(v & 0x8000000000000000ULL)) { v <<= 1; n++; } return n;
#endif
}

// log2(v) in Q8 (0 for v == 0). Max 63*256+255 = 16383.
static inline uint16_t log2Q8(uint64_t v) {
    if (!v) return 0;
    int b = 63 - clz64(v);
    uint32_t m = b >= 7 ? (uint32_t)(v >> (b - 7)) : (uint32_t)(v << (7 - b));
    return (uint16_t)(b * 256 + kT.log2Frac[m & 127]);
}


// ---------------------------------------------------------------- init

static void buildGainTables(Engine* e, const Params& p) {
    double floorLin = math::dbToLin(p.gainFloorDb);
    if (floorLin > 1.0) floorLin = 1.0;
    e->gainFloor = (int16_t)(floorLin * 32767.0 + 0.5);
    double ratio = 1.0;                                       // noise / signal power
    for (int i = 0; i < kGainLut; i++, ratio *= 0.95760328069857365) {   // * 2^(-1/16)
        double g = math::sqrtd(1.0 - p.overSub * ratio);
        if (g < floorLin) g = floorLin;
        e->gainLut[i] = (int16_t)(g * 32767.0 + 0.5);
    }
    double hopSec = (double)kH / (double)(e->sampleRate ? e->sampleRate : 48000);
    double relDb = p.releaseDbPerSec * hopSec;                // dB per frame
    e->releaseQ15 = (int16_t)(math::dbToLin(-relDb) * 32767.0 + 0.5);
    // The minimum of the smoothed log spectrum sits below the true mean noise
    // power; measured on white noise (tests/noise_signature_test.cpp): 7.65 dB
    // at 94 frames per subwindow, +0.47 dB per doubling. Compensate so the
    // signature reads as the mean noise power per bin.
    const uint32_t sub = e->subwindowFrames ? e->subwindowFrames : 94;
    const double doublings = ((int32_t)log2Q8(sub) - (int32_t)log2Q8(94)) / 256.0;
    double biasBits = (7.65 + 0.47 * doublings + p.biasDb) / 3.0102999566;
    if (biasBits < 0) biasBits = 0;
    e->biasQ8 = (uint16_t)(biasBits * 256.0 + 0.5);
}

// In-place mode: prime the output FIFO with one hop of silence, so the
// output always keeps up with the input (total delay kN samples).
inline void resetFifo(Engine* e) {
    memset(e->fifo, 0, sizeof(e->fifo));
    e->fifoHead = 0;
    e->fifoCount = kH;
}

static void resetTracking(Engine* e) {
    memset(e->in, 0, sizeof(e->in));
    memset(e->ola, 0, sizeof(e->ola));
    memset(e->out, 0, sizeof(e->out));
    memset(e->ref, 0, sizeof(e->ref));
    e->inPos = 0;
    for (int k = 0; k < kBins; k++) {
        e->smooth[k] = 0;
        e->level[k] = 0;
        e->curMin[k] = 0xFFFF;
        e->floorQ8[k] = 0;
        e->gainPrev[k] = 32767;
        for (int s = 0; s < kSub; s++) e->subMin[s][k] = 0xFFFF;
    }
    e->frameInSub = 0;
    e->subIdx = e->subFilled = 0;
    e->trackerValid = false;
    // The first frame seeds smooth[] with a raw periodogram value; keep it and
    // the next few out of the minimum until the smoothing has settled.
    e->holdFrames = 32;
    memset(&e->stats, 0, sizeof(e->stats));
    resetFifo(e);
}

static void setTiming(Engine* e, const Params& p) {
    double fps = (double)e->sampleRate / kH;
    e->subwindowFrames = (uint32_t)(p.subwindowSec * fps + 0.5);
    if (e->subwindowFrames < 8) e->subwindowFrames = 8;
    e->learnFrames = (uint32_t)(p.learnSec * fps + 0.5);
}

// Clears all state, including the signature.
inline void init(Engine* e, uint32_t sampleRate, const Params& p) {
    // sqrt-Hann window by symmetry; twiddles are window samples:
    // sin(2*pi*k/N) = w[2k], cos(2*pi*k/N) = w[2k + N/2] (k < N/4) or -w[2k - N/2].
    for (int n = 0; n <= kN / 2; n++) e->win[n] = q31(math::sin0pi(math::kPi * n / kN));
    for (int n = kN / 2 + 1; n < kN; n++) e->win[n] = e->win[kN - n];
    for (int k = 0; k < kN / 2; k++) {
        e->tws[k] = e->win[2 * k];
        e->twc[k] = k < kN / 4 ? e->win[2 * k + kN / 2] : -e->win[2 * k - kN / 2];
    }
    for (int i = 0, j = 0; i < kM; i++) {
        e->rev[i] = (uint8_t)j;
        int bit = kM >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
    }
    e->sampleRate = sampleRate;
    setTiming(e, p);
    e->smoothShift = 3;                                   // ~43 ms at 48 kHz
    buildGainTables(e, p);
    resetTracking(e);
    memset(e->sig, 0, sizeof(e->sig));
    e->sigValid = false;
    e->sigFrames = e->sigUpdates = 0;
}

// Change thresholds without losing learned state.
inline void setParams(Engine* e, const Params& p) {
    setTiming(e, p);
    buildGainTables(e, p);
}

// Seed the signature (log2 power Q8); used by tools/noise_wav.cpp.
inline void loadSignature(Engine* e, const uint16_t* q8) {
    memcpy(e->sig, q8, sizeof(e->sig));
    e->sigValid = true;
    e->sigFrames = e->learnFrames;    // counts as learned
    e->sigUpdates = 16;               // slow (1/16) refresh from the start
}

inline void clearSignature(Engine* e) {
    memset(e->sig, 0, sizeof(e->sig));
    e->sigValid = false;
    e->sigFrames = e->sigUpdates = 0;
}

// The noise itself changed upstream (filter, format, shift): forget the
// signature and the tracker minima, then pause minimum tracking for `hold`
// frames while the smoothed level and the filter settle.
inline void relearn(Engine* e, uint16_t hold) {
    for (int k = 0; k < kBins; k++) {
        e->curMin[k] = 0xFFFF;
        e->floorQ8[k] = 0;
        for (int s = 0; s < kSub; s++) e->subMin[s][k] = 0xFFFF;
    }
    e->frameInSub = 0;
    e->subIdx = e->subFilled = 0;
    e->trackerValid = false;
    clearSignature(e);
    e->holdFrames = hold;
}

inline bool signatureLearned(const Engine* e) {
    return e->sigValid && e->sigFrames >= e->learnFrames;
}

// Power of a full-scale sine landing in one bin, as log2 (not Q8), so callers
// can show levels in dB re full scale: dB = (q8/256 - ref) * 3.0103.
constexpr double kFullScaleAmp = 32767.0 * (double)(1 << kInFrac) * kWinSum / 2.0;
constexpr double kFullScaleRefLog2 = math::log2d(kFullScaleAmp * kFullScaleAmp);
inline double fullScaleRefLog2() { return kFullScaleRefLog2; }

// Sum of squared window values (for converting per-bin power to RMS).
inline double windowPowerSum() { return kWinPowerSum; }

// ---------------------------------------------------------------- FFT
//
// Every product is a single 32x32 -> high-32 multiply (one MULH on RISC-V,
// MULSH on Xtensa) with Q31 twiddles, which gives b*w/2. Both transforms
// therefore halve at every stage. Scale bookkeeping (u = input << 15):
//   forward FFT /256, split /2      -> spec = DFT(x*w) << kInFrac (= DFT(u)/512)
//   inverse split x1, inverse /256  -> buf  = x*w << kInFrac
//   synthesis window via MULH (/2)  -> ola  = x << kOutFrac (w^2 overlap sums to 1)
// Magnitudes never exceed the windowed input (< 2^30.5), so nothing saturates.

static inline int32_t mulh(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 32);
}

// In-place radix-2 DIT complex FFT of size kM, halving every stage (1/kM).
// inverse=true uses e^{+i}.
static NSIG_HOT void fft(const Engine* e, int32_t* d, bool inverse) {
    for (int i = 0; i < kM; i++) {
        const int j = e->rev[i];
        if (i < j) {
            int32_t t0 = d[2 * i], t1 = d[2 * i + 1];
            d[2 * i] = d[2 * j]; d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = t0; d[2 * j + 1] = t1;
        }
    }
    // First stage: twiddle 1, no multiplies.
    for (int i = 0; i < kM; i += 2) {
        int32_t* a = d + 2 * i;
        const int32_t ar = a[0] >> 1, ai = a[1] >> 1, br = a[2] >> 1, bi = a[3] >> 1;
        a[0] = ar + br; a[1] = ai + bi; a[2] = ar - br; a[3] = ai - bi;
    }
    for (int len = 4; len <= kM; len <<= 1) {
        const int half = len >> 1;
        const int step = kN / len;
        // k = 0: twiddle 1, no multiplies.
        for (int i = 0; i < kM; i += len) {
            int32_t* a = d + 2 * i;
            int32_t* b = d + 2 * (i + half);
            const int32_t ar = a[0] >> 1, ai = a[1] >> 1, br = b[0] >> 1, bi = b[1] >> 1;
            a[0] = ar + br; a[1] = ai + bi; b[0] = ar - br; b[1] = ai - bi;
        }
        for (int k = 1; k < half; k++) {
            const int32_t c = e->twc[k * step];
            const int32_t s = inverse ? -e->tws[k * step] : e->tws[k * step];
            for (int i = k; i < kM; i += len) {
                int32_t* a = d + 2 * i;
                int32_t* b = d + 2 * (i + half);
                const int32_t br = b[0], bi = b[1];
                // (b * (c - i s)) / 2
                const int32_t tr = mulh(br, c) + mulh(bi, s);
                const int32_t ti = mulh(bi, c) - mulh(br, s);
                const int32_t ar = a[0] >> 1, ai = a[1] >> 1;
                a[0] = ar + tr; a[1] = ai + ti;
                b[0] = ar - tr; b[1] = ai - ti;
            }
        }
    }
}

// buf (kM complex, FFT/kM of u[2n] + i u[2n+1]) -> spec[0..kM] = DFT(u)/512.
static NSIG_HOT void splitForward(Engine* e) {
    const int32_t* z = e->buf;
    int32_t* X = e->spec;
    for (int k = 0; k <= kM; k++) {
        const int a = k & (kM - 1), b = (kM - k) & (kM - 1);
        const int32_t zr = z[2 * a], zi = z[2 * a + 1];
        const int32_t cr = z[2 * b], ci = -z[2 * b + 1];          // conj(Z[M-k])
        const int32_t er = (zr >> 2) + (cr >> 2);                 // E/2
        const int32_t ei = (zi >> 2) + (ci >> 2);
        const int32_t orr = (zi >> 1) - (ci >> 1);                // O (= (Z - conj)/2i)
        const int32_t oi = (cr >> 1) - (zr >> 1);
        int32_t wr, wi;                                           // W^k * O / 2, W = c - i s
        if (k == kM) { wr = -(orr >> 1); wi = -(oi >> 1); }
        else {
            const int32_t c = e->twc[k], s = e->tws[k];
            wr = mulh(orr, c) + mulh(oi, s);
            wi = mulh(oi, c) - mulh(orr, s);
        }
        X[2 * k] = er + wr;
        X[2 * k + 1] = ei + wi;
    }
}

// spec[0..kM] -> buf (kM complex) such that the inverse FFT gives u[2n] + i u[2n+1]
// at the spec's scale.
static NSIG_HOT void splitInverse(Engine* e) {
    const int32_t* X = e->spec;
    int32_t* z = e->buf;
    for (int k = 0; k < kM; k++) {
        const int32_t ar = X[2 * k], ai = X[2 * k + 1];
        const int32_t br = X[2 * (kM - k)], bi = -X[2 * (kM - k) + 1];   // conj(X[M-k])
        const int32_t dr = ar - br, di = ai - bi;                         // 2 W^k O
        const int32_t c = e->twc[k], s = e->tws[k];
        const int32_t orr = mulh(dr, c) - mulh(di, s);                    // W^-k (A-B) / 2 = O
        const int32_t oi = mulh(di, c) + mulh(dr, s);
        // Z = E + i O, E = (A + B)/2
        z[2 * k] = (ar >> 1) + (br >> 1) - oi;
        z[2 * k + 1] = (ai >> 1) + (bi >> 1) + orr;
    }
}

// ---------------------------------------------------------------- tracking

static NSIG_HOT void track(Engine* e) {
    const int32_t* X = e->spec;
    const int sh = e->smoothShift;
    const bool hold = e->holdFrames != 0;
    if (hold) e->holdFrames--;
    for (int k = 0; k < kBins; k++) {
        const int64_t re = X[2 * k], im = X[2 * k + 1];
        const uint16_t L = log2Q8((uint64_t)(re * re) + (uint64_t)(im * im));
        e->level[k] = L;
        int32_t S = e->smooth[k];
        S = (e->stats.frames == 0) ? L : S + (((int32_t)L - S) >> sh);
        e->smooth[k] = (uint16_t)S;
        if (!hold && S < e->curMin[k]) e->curMin[k] = (uint16_t)S;
    }
    if (++e->frameInSub >= e->subwindowFrames) {
        // Close the subwindow: store its minima and refresh the floor.
        memcpy(e->subMin[e->subIdx], e->curMin, sizeof(e->curMin));
        e->subIdx = (uint8_t)((e->subIdx + 1) % kSub);
        if (e->subFilled < kSub) e->subFilled++;
        for (int k = 0; k < kBins; k++) {
            uint16_t m = 0xFFFF;
            for (int s = 0; s < kSub; s++) if (e->subMin[s][k] < m) m = e->subMin[s][k];
            uint32_t f = (uint32_t)m + e->biasQ8;
            e->floorQ8[k] = (uint16_t)(f > 0xFFFF ? 0xFFFF : f);
            e->curMin[k] = 0xFFFF;
        }
        e->frameInSub = 0;
        e->trackerValid = true;
        // Fold the tracker floor into the long-term signature: plain average
        // for the first few windows, then a slow exponential update (1/16)
        // so it keeps adapting ("periodically re-identified").
        uint32_t n = ++e->sigUpdates;
        int div = n < 16 ? (int)n : 16;
        for (int k = 0; k < kBins; k++) {
            int32_t cur = e->sigValid ? e->sig[k] : e->floorQ8[k];
            cur += ((int32_t)e->floorQ8[k] - cur) / div;
            e->sig[k] = (uint16_t)cur;
        }
        e->sigValid = true;
    }
    if (e->sigValid || e->trackerValid) e->sigFrames++;
}

// ---------------------------------------------------------------- apply

static NSIG_HOT void applyGains(Engine* e) {
    const uint16_t* ref = e->sigValid ? e->sig : (e->trackerValid ? e->floorQ8 : nullptr);
    int32_t* X = e->spec;
    for (int k = 0; k < kBins; k++) {
        int32_t g;
        if (!ref) {
            g = 32767;   // nothing learned yet: unity, but still do the work
        } else {
            // Decide on the ~40 ms smoothed level: the raw periodogram of hiss
            // jumps around (exponential distribution) and would let bursts through.
            int32_t d = (int32_t)e->smooth[k] - (int32_t)ref[k];     // SNR, log2 Q8
            if (d <= 0) g = e->gainFloor;
            else { int idx = d >> 4; g = e->gainLut[idx >= kGainLut ? kGainLut - 1 : idx]; }
        }
        int32_t rel = ((int32_t)e->gainPrev[k] * e->releaseQ15) >> 15;
        if (g < rel) g = rel;
        e->gainPrev[k] = (int16_t)g;
        X[2 * k] = (int32_t)(((int64_t)X[2 * k] * g) >> 15);
        X[2 * k + 1] = (int32_t)(((int64_t)X[2 * k + 1] * g) >> 15);
    }
}

// One frame on in[0..kN). Emits kH samples into out[] (aligned with ref[]).
static NSIG_HOT void processFrame(Engine* e, bool denoise) {
    for (int n = 0; n < kN; n++) {
        e->buf[n] = mulh((int32_t)e->in[n] * 65536, e->win[n]);   // u = x*w << 15, re/im interleaved
    }
    fft(e, e->buf, false);
    splitForward(e);
    track(e);
    if (denoise) applyGains(e);
    splitInverse(e);
    fft(e, e->buf, true);
    for (int n = 0; n < kN; n++) {
        e->ola[n] += mulh(e->buf[n], e->win[n]);
    }
    uint64_t ein = 0, eout = 0;
    for (int n = 0; n < kH; n++) {
        int16_t y = sat16((e->ola[n] + (1 << (kOutFrac - 1))) >> kOutFrac);
        e->out[n] = y;
        e->ref[n] = e->in[n];
        ein += (uint64_t)((int32_t)e->in[n] * e->in[n]);
        eout += (uint64_t)((int32_t)y * y);
    }
    for (int n = 0; n < kH; n++) { e->ola[n] = e->ola[n + kH]; e->ola[n + kH] = 0; }
    e->stats.inEnergy += ein;
    e->stats.outEnergy += eout;
    e->stats.frames++;
}

// Feed n PCM samples. Returns how many frames were processed; after each
// frame out[] holds the next kH output samples (latency kN - kH). onHop, if
// given, is called after every frame.
typedef void (*HopFn)(const Engine* e, void* ctx);
inline int process(Engine* e, const int16_t* pcm, int n, bool denoise,
                   HopFn onHop = nullptr, void* ctx = nullptr) {
    int frames = 0;
    while (n > 0) {
        int take = kH - e->inPos;
        if (take > n) take = n;
        memcpy(e->in + kH + e->inPos, pcm, (size_t)take * sizeof(int16_t));
        e->inPos += take; pcm += take; n -= take;
        if (e->inPos == kH) {
            processFrame(e, denoise);
            if (onHop) onHop(e, ctx);
            for (int i = 0; i < kH; i++) e->in[i] = e->in[i + kH];
            e->inPos = 0;
            frames++;
        }
    }
    return frames;
}

// Process n samples in place: pcm is overwritten with the processed output,
// delayed by exactly kN samples (10.7 ms at 48 kHz). Any n; only a 3-hop FIFO
// is needed because output is written back only behind the read position.
inline int processInPlace(Engine* e, int16_t* pcm, int n, bool denoise) {
    const int cap = 3 * kH;
    int r = 0, w = 0, frames = 0;
    while (r < n) {
        int take = kH - e->inPos;
        if (take > n - r) take = n - r;
        if (process(e, pcm + r, take, denoise)) {           // at most one frame
            frames++;
            for (int i = 0; i < kH; i++) {
                e->fifo[(e->fifoHead + e->fifoCount) % cap] = e->out[i];
                e->fifoCount++;
            }
        }
        r += take;
        // write back behind the read position
        while (w < r && e->fifoCount > 0) {
            pcm[w++] = e->fifo[e->fifoHead];
            e->fifoHead = (e->fifoHead + 1) % cap;
            e->fifoCount--;
        }
    }
    return frames;
}

}  // namespace nsig
