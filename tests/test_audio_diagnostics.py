from webui_source import webui_source
from firmware_source import firmware_source
"""Run diagnostics helpers and the actual ISR/API integration on the host."""
import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = firmware_source(ROOT)
WEB = webui_source(ROOT)


def function(source, signature):
    start = source.index(signature + ' {')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class AudioDiagnosticsTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            return subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10).stdout

    def test_load_window_and_recent_event_boundaries(self):
        self.compile_run(r'''
#include "AudioDiagnostics.h"
#include <cassert>
int main() {
    AudioProcessingWindow load;
    load.reset(0);
    assert(load.snapshot() == 0);
    assert(!load.add(1000000, 200000));
    assert(load.add(2000000, 200000));
    auto snapshot = load.snapshot();
    assert(snapshot & AudioProcessingWindow::validBit);
    assert(AudioProcessingWindow::currentX10(snapshot) == 200); // 20%, excludes read waits.
    assert(AudioProcessingWindow::peakX10(snapshot) == 200);
    assert(load.add(4000000, 100000));
    assert(AudioProcessingWindow::currentX10(load.snapshot()) == 50);
    assert(AudioProcessingWindow::peakX10(load.snapshot()) == 200);
    assert(load.add(6000000, 4000000)); // Scheduling delay can inflate this estimate.
    assert(AudioProcessingWindow::currentX10(load.snapshot()) == 1000);
    load.reset(uint64_t(UINT32_MAX) - 1000000);
    assert(load.snapshot() == 0);
    assert(load.add(uint64_t(UINT32_MAX) + 1000000, 200000));
    assert(AudioProcessingWindow::currentX10(load.snapshot()) == 100);
    RecentDmaOverruns recent;
    recent.reset(0);
    assert(recent.update(999, 5) == 0);
    assert(recent.update(1000, 5) == 5); // Include events before the first poll.
    assert(recent.count() == 5 && recent.hasEvent() && recent.lastEventMs() == 1000);
    for (uint32_t t = 2000; t <= 60000; t += 1000) recent.update(t, 5);
    assert(recent.count() == 5);
    recent.update(61000, 5);
    assert(recent.count() == 0 && recent.hasEvent());
    recent.update(181000, 8); // Long gap: old bins expire, new observed increment remains.
    assert(recent.count() == 3);
    recent.reset(UINT32_MAX - 500, UINT32_MAX - 2);
    assert(recent.update(1000, 2) == 5); // Both millis and the total wrap.
    assert(recent.count() == 5);
    recent.reset(0);
    uint32_t total = 0;
    for (uint32_t t = 1000; t <= 120000; t += 1000) {
        recent.update(t, ++total);
        assert(recent.count() == (t < 60000 ? t / 1000 : 60));
    }
}
''')

    def test_actual_isr_registration_counting_and_stop_gating(self):
        code = r'''
#include "AudioDiagnostics.h"
#include <cassert>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define IRAM_ATTR
#define portENTER_CRITICAL(mux) ((mux)->lock())
#define portEXIT_CRITICAL(mux) ((mux)->unlock())
#define portENTER_CRITICAL_ISR(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux) portEXIT_CRITICAL(mux)
struct String : std::string {
    using std::string::string;
    String(const std::string& s): std::string(s) {}
    String(uint32_t value): std::string(std::to_string(value)) {}
};
using i2s_chan_handle_t = void*;
struct i2s_event_data_t {};
struct i2s_event_callbacks_t { bool (*on_recv_q_ovf)(i2s_chan_handle_t, i2s_event_data_t*, void*) = nullptr; };
using esp_err_t = int;
constexpr int ESP_OK = 0;
std::mutex audioDmaMux;
uint32_t audioDmaOverrunTotal = 0, now = 1000;
bool audioDmaCountingEnabled = false, audioDmaCallbackAvailable = false;
bool audioProducerStopRequested = false;
RecentDmaOverruns audioDmaWindow;
i2s_chan_handle_t i2s_rx_handle = nullptr;
int registrationResult = 0, enableResult = 0, logs = 0;
std::vector<int> calls;
uint32_t millis() { return now; }
const char* esp_err_to_name(int) { return "test error"; }
void simplePrintln(const String&) { ++logs; }
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t* cb, void*) {
    assert(cb->on_recv_q_ovf); calls.push_back(1); return registrationResult;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t) { calls.push_back(2); return enableResult; }
'''
        for signature in ['static bool IRAM_ATTR onI2sDmaOverrun(i2s_chan_handle_t, i2s_event_data_t*, void*)',
                          'static void setI2sDmaCounting(bool enabled)', 'uint32_t getI2sDmaOverrunTotal()',
                          'static void registerI2sDiagnostics()', 'static esp_err_t enableI2sReceiverWithDiagnostics()',
                          'DmaOverrunStatus getI2sDmaStatus()', 'void updateAudioDiagnostics()']:
            code += function(SOURCE, signature) + '\n'
        code += r'''
int main() {
    assert(enableI2sReceiverWithDiagnostics() == ESP_OK && audioDmaCallbackAvailable);
    assert((calls == std::vector<int>{1, 2}));
    registrationResult = -1; calls.clear();
    assert(enableI2sReceiverWithDiagnostics() == ESP_OK && !audioDmaCallbackAvailable);
    assert((calls == std::vector<int>{1, 2})); // Diagnostic failure does not disable audio.
    registrationResult = 0; enableResult = -1;
    assert(enableI2sReceiverWithDiagnostics() == -1 && !audioDmaCallbackAvailable);
    enableResult = 0; assert(enableI2sReceiverWithDiagnostics() == ESP_OK);
    assert(!onI2sDmaOverrun(nullptr, nullptr, nullptr));
    assert(getI2sDmaOverrunTotal() == 0); // Ignore configuration/startup callbacks.
    setI2sDmaCounting(true);
    std::thread isr([] { for (int i = 0; i < 20000; ++i) onI2sDmaOverrun(nullptr, nullptr, nullptr); });
    uint32_t prior = 0;
    for (int i = 0; i < 20000; ++i) { uint32_t value = getI2sDmaOverrunTotal(); assert(value >= prior); prior = value; }
    isr.join();
    assert(getI2sDmaOverrunTotal() == 20000);
    audioProducerStopRequested = true; setI2sDmaCounting(true);
    onI2sDmaOverrun(nullptr, nullptr, nullptr);
    assert(getI2sDmaOverrunTotal() == 20000); // Cannot re-arm during stop.
    audioProducerStopRequested = false;
    int before = logs;
    audioDmaWindow.reset(0); updateAudioDiagnostics();
    assert(logs == before + 1 && getI2sDmaStatus().recent == 20000);
    setI2sDmaCounting(true); onI2sDmaOverrun(nullptr, nullptr, nullptr);
    now = 2000; updateAudioDiagnostics();
    assert(logs == before + 1); // Rate limit repeated reports.
    now = 61000; onI2sDmaOverrun(nullptr, nullptr, nullptr); updateAudioDiagnostics();
    assert(logs == before + 2);
    setI2sDmaCounting(false);
    onI2sDmaOverrun(nullptr, nullptr, nullptr);
    assert(getI2sDmaStatus().total == 20002);
    audioDmaOverrunTotal = UINT32_MAX;
    setI2sDmaCounting(true); onI2sDmaOverrun(nullptr, nullptr, nullptr);
    assert(getI2sDmaOverrunTotal() == 0);
}
'''
        self.compile_run(code)

    def test_actual_api_marks_stopped_stale_and_unavailable_metrics(self):
        code = r'''
#include "AudioDiagnostics.h"
#include <atomic>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
struct String : std::string {
    using std::string::string;
    String(const std::string& s): std::string(s) {}
    String(uint32_t value): std::string(std::to_string(value)) {}
    String(float value, int places) {
        std::ostringstream out; out << std::fixed << std::setprecision(places) << value; assign(out.str());
    }
};
bool audioProducerRunning = true, audioProducerStopRequested = false;
uint32_t now = 5000, audioProducerLastReadMs = 4990;
uint32_t millis() { return now; }
std::atomic<uint32_t> audioProcessingLoadSnapshot{AudioProcessingWindow::validBit | 123 | (456 << 10)};
DmaOverrunStatus dma{true, 7, 2, true, 4000};
DmaOverrunStatus getI2sDmaStatus() { return dma; }
String formatUptime(uint32_t sec) { return std::to_string(sec) + "s"; }
String jsonEscape(const String& text) { return text; }
'''
        code += function(WEB, 'static void appendAudioDiagnosticsJson(String& json)')
        code += r'''
void report() { String json = "{"; appendAudioDiagnosticsJson(json); json += "\"end\":true}"; std::cout << json << '\n'; }
int main() {
    report();
    audioProducerRunning = false; dma.available = false; report();
    audioProducerRunning = true; now = 8000; report(); // Read stalled: stale load is unavailable.
    audioProducerLastReadMs = now; audioProcessingLoadSnapshot = 0; report(); // First window incomplete.
    audioProcessingLoadSnapshot = AudioProcessingWindow::validBit; report(); // Valid zero remains zero.
    audioProducerStopRequested = true; report();
}
'''
        data = [json.loads(line) for line in self.compile_run(code).splitlines()]
        self.assertEqual(data[0]['audio_cpu_pct'], 12.3)
        self.assertEqual(data[0]['audio_cpu_peak_pct'], 45.6)
        self.assertEqual(data[0]['dma_overruns_total'], 7)
        self.assertEqual(data[0]['dma_overruns_1min'], 2)
        for row in [data[1], data[2], data[3], data[5]]:
            self.assertFalse(row['audio_cpu_valid'])
            self.assertIsNone(row['audio_cpu_pct'])
        self.assertIsNone(data[1]['dma_overruns_total'])
        self.assertIsNone(data[1]['dma_overruns_1min'])
        self.assertTrue(data[4]['audio_cpu_valid'])
        self.assertEqual(data[4]['audio_cpu_pct'], 0)
