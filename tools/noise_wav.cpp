// Run the firmware's noise-signature engine (NoiseSignature.h) on a WAV file
// so the denoise can be heard and fed to BirdNET before it ever touches the
// RTSP stream. Same integer code as the ESP32.
//
// Build: g++ -std=c++17 -O2 -Iesp32-birdnet-mic tools/noise_wav.cpp -o noise_wav
// Use:   ./noise_wav input.wav output.wav [floor_db] [oversub]
//          floor_db  most a bin is attenuated (default -12)
//          oversub   subtraction strength (default 1.0)
// Input: 16-bit PCM WAV, mono or stereo (left channel is used).
// Two passes: the first learns the signature over the whole file, the second
// applies it from the start, so the beginning is denoised too. A noise-only
// CSV (signature in dBFS per bin) is written next to the output.

#include "NoiseSignature.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static nsig::Engine eng;

static bool readWav(const char* path, std::vector<int16_t>& pcm, uint32_t& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    uint16_t ch = 0, bits = 0, fmt = 0;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        uint32_t sz; std::memcpy(&sz, &d[p + 4], 4);
        if (!std::memcmp(&d[p], "fmt ", 4)) {
            std::memcpy(&fmt, &d[p + 8], 2); std::memcpy(&ch, &d[p + 10], 2);
            std::memcpy(&rate, &d[p + 12], 4); std::memcpy(&bits, &d[p + 22], 2);
        } else if (!std::memcmp(&d[p], "data", 4)) {
            if (fmt != 1 || bits != 16 || ch < 1) { std::fprintf(stderr, "need 16-bit PCM WAV\n"); return false; }
            size_t frames = std::min<size_t>(sz, d.size() - p - 8) / (2 * ch);
            pcm.resize(frames);
            for (size_t i = 0; i < frames; i++) std::memcpy(&pcm[i], &d[p + 8 + i * 2 * ch], 2);
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

static bool writeWav(const char* path, const std::vector<int16_t>& pcm, uint32_t rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t dataBytes = (uint32_t)pcm.size() * 2, riff = 36 + dataBytes, fmtLen = 16, byteRate = rate * 2;
    uint16_t pcmFmt = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcmFmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    return std::fclose(f) == 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.wav out.wav [floor_db=-12] [oversub=1.0]\n", argv[0]);
        return 2;
    }
    std::vector<int16_t> x;
    uint32_t rate = 0;
    if (!readWav(argv[1], x, rate)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    nsig::Params p;
    if (argc > 3) p.gainFloorDb = (float)std::atof(argv[3]);
    if (argc > 4) p.overSub = (float)std::atof(argv[4]);
    double seconds = (double)x.size() / rate;
    if (seconds < 8 * p.subwindowSec * 1.2) {
        p.subwindowSec = (float)std::max(0.5, seconds / 10.0);   // short clip: shorter windows
        std::fprintf(stderr, "short clip: subwindow %.1f s\n", p.subwindowSec);
    }

    // Pass 1: learn.
    nsig::init(&eng, rate, p);
    nsig::process(&eng, x.data(), (int)x.size(), false);
    if (!eng.sigValid) { std::fprintf(stderr, "clip too short to learn a signature\n"); return 1; }
    std::vector<uint16_t> sig(eng.sig, eng.sig + nsig::kBins);

    // Pass 2: apply from the start with the learned signature.
    nsig::init(&eng, rate, p);
    nsig::loadSignature(&eng, sig.data());
    std::vector<int16_t> y;
    y.reserve(x.size() + nsig::kN);
    std::vector<int16_t> padded(x);
    padded.insert(padded.end(), nsig::kN, 0);   // flush the latency
    for (size_t pos = 0; pos + nsig::kH <= padded.size(); pos += nsig::kH) {
        nsig::process(&eng, padded.data() + pos, nsig::kH, true);
        y.insert(y.end(), eng.out, eng.out + nsig::kH);
    }
    const size_t lat = nsig::kN - nsig::kH;
    std::vector<int16_t> out(y.begin() + lat, y.begin() + lat + x.size());
    if (!writeWav(argv[2], out, rate)) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }

    double ein = 0, eout = 0;
    for (size_t i = 0; i < x.size(); i++) { ein += (double)x[i] * x[i]; eout += (double)out[i] * out[i]; }
    std::printf("%s: %.1f s at %u Hz, overall level change %.2f dB\n", argv[2], seconds, rate,
                10 * std::log10((eout + 1) / (ein + 1)));

    std::string csv = std::string(argv[2]) + ".signature.csv";
    if (FILE* f = std::fopen(csv.c_str(), "w")) {
        double ref = nsig::fullScaleRefLog2();
        std::fprintf(f, "bin,freq_hz,signature_db\n");
        for (int k = 0; k < nsig::kBins; k++)
            std::fprintf(f, "%d,%.1f,%.2f\n", k, k * (double)rate / nsig::kN, (sig[k] / 256.0 - ref) * 3.0103);
        std::fclose(f);
        std::printf("signature: %s\n", csv.c_str());
    }
    return 0;
}
