// Host test for NoiseSignature.h (fixed-point FFT, noise floor tracking,
// spectral gain, overlap-add).
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iesp32-birdnet-mic tests/noise_signature_test.cpp

#include "NoiseSignature.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdio>
#include <random>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what, double v = NAN) {
    if (std::isnan(v)) std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    else std::printf("%s %s (%.3f)\n", ok ? "PASS" : "FAIL", what, v);
    if (!ok) failures++;
}

static nsig::Engine eng;   // ~25 KB: keep off the stack

static std::vector<int16_t> runAll(nsig::Engine* e, const std::vector<int16_t>& x, bool denoise) {
    std::vector<int16_t> y;
    y.reserve(x.size());
    size_t pos = 0;
    while (pos < x.size()) {
        int n = (int)std::min<size_t>(201, x.size() - pos);   // odd size, < hop on purpose
        int before = (int)e->stats.frames;
        nsig::process(e, x.data() + pos, n, denoise);
        int after = (int)e->stats.frames;
        if (after > before) {
            // only the latest hop is kept in out[]; chunk < hop so at most one frame
            y.insert(y.end(), e->out, e->out + nsig::kH);
        }
        pos += n;
    }
    return y;
}

static double bandPowerDb(const std::vector<int16_t>& s, size_t start, size_t len,
                          double fs, double f0, double f1) {
    // Hann-windowed periodogram average over 4096-sample blocks
    const int B = 4096;
    double acc = 0; int blocks = 0;
    for (size_t off = start; off + B <= start + len && off + B <= s.size(); off += B) {
        std::vector<std::complex<double>> X(B);
        for (int k = (int)(f0 * B / fs); k <= (int)(f1 * B / fs); k++) {
            std::complex<double> sum = 0;
            for (int n = 0; n < B; n++) {
                double w = 0.5 - 0.5 * std::cos(2 * M_PI * n / B);
                sum += w * s[off + n] * std::polar(1.0, -2 * M_PI * k * n / B);
            }
            acc += std::norm(sum);
        }
        blocks++;
    }
    return 10 * std::log10(acc / std::max(blocks, 1) + 1e-30);
}

int main() {
    const double fs = 48000;
    nsig::Params p;

    // 1. Forward FFT accuracy against a double DFT of the same windowed frame.
    {
        nsig::init(&eng, (uint32_t)fs, p);
        std::mt19937 rng(1);
        std::normal_distribution<double> g(0, 3000);
        std::vector<int16_t> x(nsig::kN);
        for (auto& v : x) v = (int16_t)std::max(-32768.0, std::min(32767.0, g(rng)));
        nsig::process(&eng, x.data(), nsig::kH, false);       // fills second half
        nsig::process(&eng, x.data() + nsig::kH, nsig::kH, false);
        // After the 2nd frame, spec[] is the DFT of x windowed (frame = x[0..N)).
        double worst = 0, ref = 0;
        for (int k = 0; k < nsig::kBins; k++) {
            std::complex<double> sum = 0;
            for (int n = 0; n < nsig::kN; n++) {
                double xw = x[n] * (double)eng.win[n] / 2147483648.0 * (1 << nsig::kInFrac);
                sum += xw * std::polar(1.0, -2 * M_PI * k * n / nsig::kN);
            }
            std::complex<double> got(eng.spec[2 * k], eng.spec[2 * k + 1]);
            worst = std::max(worst, std::abs(got - sum));
            ref = std::max(ref, std::abs(sum));
        }
        check(worst / ref < 1e-5, "forward real FFT matches double DFT (max rel err x1e6)", worst / ref * 1e6);
    }

    // 2. Perfect reconstruction with denoise off, across levels.
    for (double level : {5.0, 300.0, 12000.0}) {
        nsig::init(&eng, (uint32_t)fs, p);
        std::mt19937 rng(2);
        std::normal_distribution<double> g(0, level);
        std::vector<int16_t> x(48000);
        for (auto& v : x) v = (int16_t)std::max(-32768.0, std::min(32767.0, g(rng)));
        auto y = runAll(&eng, x, false);
        int worst = 0;
        for (size_t i = nsig::kN; i < y.size(); i++) {
            int d = std::abs((int)y[i] - (int)x[i - (nsig::kN - nsig::kH)]);
            worst = std::max(worst, d);
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "unity reconstruction within 1 LSB, sigma=%g", level);
        check(worst <= 1, msg, worst);
    }

    // 3. Full-scale square wave: no overflow, still reconstructs.
    {
        nsig::init(&eng, (uint32_t)fs, p);
        std::vector<int16_t> x(24000);
        for (size_t i = 0; i < x.size(); i++) x[i] = ((i / 37) & 1) ? 32767 : -32768;
        auto y = runAll(&eng, x, false);
        int worst = 0;
        for (size_t i = nsig::kN; i < y.size(); i++)
            worst = std::max(worst, std::abs((int)y[i] - (int)x[i - (nsig::kN - nsig::kH)]));
        check(worst <= 2, "full-scale square wave reconstructs (no overflow)", worst);
    }

    // 4. Learned floor of white noise vs its true per-bin power.
    {
        nsig::Params q = p; q.subwindowSec = 1.0f; q.learnSec = 20.0f;
        nsig::init(&eng, (uint32_t)fs, q);
        std::mt19937 rng(3);
        const double sigma = 40.0;
        std::normal_distribution<double> g(0, sigma);
        std::vector<int16_t> x((size_t)(fs * 30));
        for (auto& v : x) v = (int16_t)std::lround(g(rng));
        runAll(&eng, x, false);
        // Expected per-bin power of windowed white noise: sigma^2 * sum(w^2) * 2^(2*kInFrac)
        double sw2 = 0;
        for (int n = 0; n < nsig::kN; n++) sw2 += std::pow(eng.win[n] / 2147483648.0, 2);
        double expectQ8 = 256.0 * std::log2(sigma * sigma * sw2 * std::pow(2.0, 2 * nsig::kInFrac));
        std::vector<double> diffDb;
        for (int k = 4; k < nsig::kBins - 4; k++)
            diffDb.push_back((eng.sig[k] - expectQ8) / 256.0 * 3.0103);
        std::sort(diffDb.begin(), diffDb.end());
        double med = diffDb[diffDb.size() / 2];
        double spread = diffDb[diffDb.size() * 9 / 10] - diffDb[diffDb.size() / 10];
        check(nsig::signatureLearned(&eng), "signature marked learned after learnSec");
        check(med > -1.5 && med < 3.0, "white-noise signature median vs true level, dB", med);
        check(spread < 3.0, "signature 10-90% spread across bins, dB", spread);
    }

    // 5. Tone + hiss + intermittent chirps: hiss reduced, tone/chirps kept.
    {
        nsig::Params q = p; q.subwindowSec = 1.0f;
        nsig::init(&eng, (uint32_t)fs, q);
        std::mt19937 rng(4);
        std::normal_distribution<double> g(0, 40.0);
        const size_t total = (size_t)(fs * 40);
        std::vector<int16_t> x(total);
        for (size_t i = 0; i < total; i++) {
            double t = i / fs;
            double v = g(rng) + 1500.0 * std::sin(2 * M_PI * 3000.0 * t);
            // a 0.3 s chirp 5-6 kHz every 2 s ("bird")
            double ph = std::fmod(t, 2.0);
            if (ph < 0.3) v += 3000.0 * std::sin(2 * M_PI * (5000.0 + 3333.0 * ph) * ph);
            x[i] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v)));
        }
        auto y = runAll(&eng, x, true);
        const size_t lat = nsig::kN - nsig::kH;
        // Compare the last 10 s (signature learned), aligned for latency.
        size_t start = total - (size_t)(fs * 10);
        std::vector<int16_t> xa(x.begin() + start - lat, x.end() - lat);
        std::vector<int16_t> ya(y.begin() + start, y.begin() + start + xa.size());
        double hissIn = bandPowerDb(xa, 0, xa.size(), fs, 9000, 11000);
        double hissOut = bandPowerDb(ya, 0, ya.size(), fs, 9000, 11000);
        double toneIn = bandPowerDb(xa, 0, xa.size(), fs, 2950, 3050);
        double toneOut = bandPowerDb(ya, 0, ya.size(), fs, 2950, 3050);
        check(hissIn - hissOut > 6.0, "hiss band (9-11 kHz) reduced, dB", hissIn - hissOut);
        // A steady tone is indistinguishable from hum/whine: it should be in the
        // signature (that is how such noise sources show up) and be attenuated.
        const int toneBin = (int)std::lround(3000.0 * nsig::kN / fs);
        double toneAbove = (eng.sig[toneBin] - eng.sig[toneBin + 6]) / 256.0 * 3.0103;
        check(toneAbove > 20.0, "steady 3 kHz tone stands out in the signature, dB", toneAbove);
        check(toneIn - toneOut > 6.0, "steady 3 kHz tone attenuated, dB", toneIn - toneOut);
        // Chirps: 5-6 kHz band, chirp segments only.
        std::vector<int16_t> cIn, cOut;
        for (size_t i = 0; i < xa.size(); i++) {
            double t = (start - lat + i) / fs;
            if (std::fmod(t, 2.0) < 0.3) { cIn.push_back(xa[i]); cOut.push_back(ya[i]); }
        }
        double chDb = bandPowerDb(cOut, 0, cOut.size(), fs, 5000, 6000) -
                      bandPowerDb(cIn, 0, cIn.size(), fs, 5000, 6000);
        check(chDb > -1.0, "intermittent chirps kept (5-6 kHz during chirps, dB change)", chDb);
        std::printf("INFO hiss -%.2f dB, tone -%.2f dB, chirps %+.2f dB\n",
                    hissIn - hissOut, toneIn - toneOut, chDb);
    }

    // 6. In-place (apply) mode: any block size, output = input delayed by kN.
    {
        nsig::init(&eng, (uint32_t)fs, p);
        std::mt19937 rng(9);
        std::normal_distribution<double> g(0, 3000);
        std::uniform_int_distribution<int> blk(1, 8192);
        std::vector<int16_t> x(48000 * 3), y;
        for (auto& v : x) v = (int16_t)std::max(-32768.0, std::min(32767.0, g(rng)));
        size_t pos = 0;
        while (pos < x.size()) {
            size_t n = std::min<size_t>((size_t)blk(rng), x.size() - pos);
            std::vector<int16_t> b(x.begin() + pos, x.begin() + pos + n);
            nsig::processInPlace(&eng, b.data(), (int)n, false);
            y.insert(y.end(), b.begin(), b.end());
            pos += n;
        }
        bool lenOk = y.size() == x.size();
        int worst = 0;
        for (size_t i = nsig::kN; i < y.size(); i++)
            worst = std::max(worst, std::abs((int)y[i] - (int)x[i - nsig::kN]));
        bool leadSilent = true;
        for (int i = 0; i < nsig::kH; i++) leadSilent = leadSilent && y[i] == 0;
        check(lenOk && leadSilent && worst <= 1, "in-place mode: random blocks, delay exactly kN, within 1 LSB", worst);
        check(eng.fifoCount >= 0 && eng.fifoCount < 3 * nsig::kH, "in-place FIFO stays within 3 hops", eng.fifoCount);
    }

    // 7. Long continuous song (90 s, 5-6 kHz) after 2 min of plain hiss: the
    //    fast setting starts treating it as background, the default does not.
    {
        double rise[2];
        const float subs[2] = {4.0f, 15.0f};
        for (int v = 0; v < 2; v++) {
            nsig::Params q = p; q.subwindowSec = subs[v];
            nsig::init(&eng, (uint32_t)fs, q);
            std::mt19937 rng(11);
            std::normal_distribution<double> g(0, 40.0);
            const size_t total = (size_t)(fs * 210);
            std::vector<int16_t> blk(4800);
            double ph = 0;
            for (size_t pos = 0; pos < total; pos += blk.size()) {
                for (size_t i = 0; i < blk.size(); i++) {
                    double t = (pos + i) / fs, val = g(rng);
                    if (t >= 120.0) {   // warbling song, never silent
                        ph += 2 * M_PI * (5500.0 + 400.0 * std::sin(2 * M_PI * 3.0 * t)) / fs;
                        val += 2000.0 * std::sin(ph);
                    }
                    blk[i] = (int16_t)std::lround(val);
                }
                nsig::process(&eng, blk.data(), (int)blk.size(), true);
            }
            double sum = 0; int cnt = 0;
            for (int k = (int)(5200 * nsig::kN / fs); k <= (int)(5800 * nsig::kN / fs); k++, cnt++)
                sum += eng.sig[k] / 256.0 * 3.0103;
            double ref = 0; int rc = 0;
            for (int k = (int)(9000 * nsig::kN / fs); k <= (int)(11000 * nsig::kN / fs); k++, rc++)
                ref += eng.sig[k] / 256.0 * 3.0103;
            rise[v] = sum / cnt - ref / rc;   // song band vs hiss band, in the signature
        }
        std::printf("INFO song band raised in signature: fast %.1f dB, normal %.1f dB\n", rise[0], rise[1]);
        check(rise[0] > 5.0, "fast adapt (32 s window) starts learning a 90 s song as background, dB", rise[0]);
        check(rise[1] < 3.0, "normal adapt (2 min window) keeps the 90 s song out, dB", rise[1]);
    }

    // 8. Settings change: relearn forgets signature and minima; the settling
    //     hold keeps a silent transient out of the floor.
    {
        nsig::Params q = p; q.subwindowSec = 4.0f; q.learnSec = 8.0f;
        std::mt19937 rng(31);
        std::normal_distribution<double> g(0, 1.0);
        std::vector<int16_t> blk(960);
        auto run = [&](double sigma, double seconds) {
            for (size_t pos = 0; pos < (size_t)(fs * seconds); pos += blk.size()) {
                for (auto& v : blk) v = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(sigma * g(rng))));
                nsig::process(&eng, blk.data(), (int)blk.size(), true);
            }
        };
        const int k0 = (int)(5000.0 * nsig::kN / fs), k1 = (int)(15000.0 * nsig::kN / fs);
        auto bandDb = [&](const uint16_t* a) {
            double sum = 0; for (int k = k0; k <= k1; k++) sum += a[k]; return sum / (k1 - k0 + 1) / 256.0 * 3.0103;
        };
        nsig::init(&eng, (uint32_t)fs, q);
        run(40.0, 40);
        const double fresh = bandDb(eng.sig);
        double relearned[2], old = 0;
        for (int v = 0; v < 2; v++) {
            nsig::init(&eng, (uint32_t)fs, q);
            run(400.0, 40);                                    // loud old chain
            old = bandDb(eng.sig);
            nsig::relearn(&eng, v ? 96 : 0);
            if (v) check(!eng.sigValid && !eng.trackerValid && !nsig::signatureLearned(&eng),
                         "relearn forgets signature and tracker");
            run(0.0, 0.1);                                     // settling gap (silence)
            run(40.0, 40);                                     // quieter new chain
            relearned[v] = bandDb(eng.sig);
        }
        std::printf("INFO relearn 5-15 kHz: old %.1f, fresh %.1f, with hold %.1f, without %.1f dB\n",
                    old, fresh, relearned[1], relearned[0]);
        check(std::fabs(relearned[1] - fresh) < 0.5, "relearned floor matches a fresh start (5-15 kHz), dB", relearned[1] - fresh);
        check(old - relearned[1] > 15.0, "old, louder floor forgotten, dB", old - relearned[1]);
        check(fresh - relearned[0] > 3.0, "without the hold a silent gap drags the floor down, dB", fresh - relearned[0]);
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
