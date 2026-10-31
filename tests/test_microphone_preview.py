"""Exercise the signal detector, preview queue and actual nonblocking HTTP sender."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MicrophonePreviewTests(unittest.TestCase):
    def test_actual_producer_before_gain_and_filter_for_i2s_and_both_pdm_paths(self):
        source = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
        start = source.index('        // Hardware PDM is packed int16.', source.index('void audioProducerTask('))
        end = source.index('        if (lastPeakAbs16 > peakHoldAbs16)', start)
        helper_start = source.index('void updateHighpassCoeffs() {')
        helper_end = source.index('// Uptime ->', helper_start)
        helper = source[helper_start:helper_end]
        code = r'''
#include "MicrophoneSignal.h"
#include "FixedAudio.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
bool pdm = false, MIC_PDM_HARDWARE = false, highpassEnabled = false;
uint8_t i2sShiftBits = 12;
float currentGainFactor = 0;
int samplesRead = 5;
int32_t input[5]; int32_t* i2s_32bit_buffer = input;
int16_t i2s_16bit_buffer[5];
uint16_t lastPeakAbs16; bool audioClippedLastBlock; uint32_t audioClipCount;
uint32_t now = 0;
uint32_t millis() { return now; }
MicrophoneSignal microphoneSignal;
fxaudio::BiquadQ28 hpf{};
bool hpfConfigEnabled=false;
uint32_t currentSampleRate=48000, hpfConfigSampleRate=0;
uint16_t highpassCutoffHz=500,hpfConfigCutoff=0;
uint16_t normalizeHighpassCutoff(uint32_t rate, uint16_t cutoff) {
 return std::min<uint32_t>(rate*45/100, std::max<uint16_t>(10,cutoff));
}
''' + helper + r'''
namespace recording { inline void push(const int16_t*, size_t) {} }
std::vector<int16_t> preview;
void browserAudioPush(const int16_t* data, size_t count) { preview.assign(data, data + count); }
void process() {
''' + source[start:end] + r'''
}
int main() {
 for (int mode = 0; mode < 4; ++mode) {
  pdm = mode >= 2; MIC_PDM_HARDWARE = mode == 2;
  const int16_t pcm[] = {-1234, -100, 0, 100, 1234};
  auto refill = [&] {
   std::fill(input, input + 5, 0);
   if (pdm && MIC_PDM_HARDWARE) memcpy(input, pcm, sizeof(pcm));
   else for (int i = 0; i < 5; ++i) input[i] = int32_t(pcm[i]) * (pdm ? 65536 : 4096);
  };
  refill();
  microphoneSignal.reset(); now = 100;
  currentGainFactor = 0; highpassEnabled = true;
  process();
  assert(microphoneSignal.state(now) == MicrophoneSignal::Healthy);
  assert(preview == std::vector<int16_t>(5, 0)); // Gain/filter cannot trigger a false flatline.
  currentGainFactor = 1; highpassEnabled = false; refill(); process();
  assert(std::equal(preview.begin(), preview.end(), pcm)); // Listen taps post-conversion PCM.
  std::fill(input, input + 5, 0); process();
  now += 20000; process();
  assert(microphoneSignal.state(now) == MicrophoneSignal::Flatline);
 }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'capture.cpp', pathlib.Path(temp) / 'capture'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def native(self, name, transport=False, c5=False):
        with tempfile.TemporaryDirectory() as temp:
            # Exercise sdkconfig inclusion, as in BrowserAudio.cpp on the board.
            if c5:
                (pathlib.Path(temp) / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET_ESP32C5 1\n')
            command = ['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-fsanitize=undefined', '-fno-sanitize-recover=all',
                       '-I', temp,
                       '-I', str(ROOT / 'tests/browser_audio_stubs'),
                       '-I', str(ROOT / 'esp32-birdnet-mic'), str(ROOT / 'tests' / name)]
            if transport:
                command.append(str(ROOT / 'esp32-birdnet-mic/BrowserAudio.cpp'))
            binary = str(pathlib.Path(temp) / 'test')
            subprocess.run(command + ['-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=30)

    def test_health_and_cross_task_queue(self):
        self.native('microphone_preview_test.cpp')

    def test_actual_http_transport(self):
        self.native('browser_audio_transport_test.cpp', transport=True)

    def test_c5_queue_wrap_overflow_and_cross_task_pcm(self):
        self.native('microphone_preview_test.cpp', c5=True)

    def test_c5_actual_http_transport_and_slow_listener(self):
        self.native('browser_audio_transport_test.cpp', transport=True, c5=True)

    def test_browser_playback_lifecycle(self):
        subprocess.run(['node', str(ROOT / 'tests/browser_audio_ui_test.js')], check=True, timeout=15)

    def test_browser_network_recovery_and_cancellation(self):
        subprocess.run(['node', str(ROOT / 'tests/browser_audio_recovery_test.js')], check=True, timeout=15)
