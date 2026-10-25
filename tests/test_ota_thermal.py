"""Simulate the actual OTA lifecycle and temperature-driver failure modes."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()


def function(signature):
    start = SOURCE.index(signature + ' {')
    brace = SOURCE.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


class OtaThermalTests(unittest.TestCase):
    def run_cpp(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text('namespace recording { inline bool mode(){return false;} inline void requestStop(){} inline bool stop(){return true;} }\n' + code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_range_changes_and_finite_values_from_failed_reads(self):
        self.run_cpp(r'''
#include "TemperatureReader.h"
#include <cassert>
struct Driver {
    float temperature = 40;
    bool fail = false;
    int minimum = 0, maximum = 0, selects = 0;
    bool select(int low, int high) { minimum = low; maximum = high; ++selects; return true; }
    bool read(float& value) {
        value = fail ? 110 : temperature; // Driver may write a number on failure.
        return !fail && temperature >= minimum && temperature <= maximum;
    }
};
int main() {
    Driver driver;
    TemperatureReader<Driver> reader(driver);
    assert(reader.read() == 40 && driver.selects == 1);
    driver.temperature = 85; // Above Arduino's default sensor range.
    assert(reader.read() == 85 && driver.selects == 1);
    driver.temperature = 110;
    assert(reader.read() == 110 && driver.minimum == 50);
    driver.temperature = -20;
    assert(reader.read() == -20 && driver.maximum == 20);
    driver.fail = true;
    assert(isnan(reader.read())); // Never interpret a failed read as 110 C.
    driver.fail = false; driver.temperature = 70;
    assert(reader.read() == 70);
    driver.temperature = 200;
    assert(isnan(reader.read()));
    driver.temperature = NAN;
    assert(isnan(reader.read()));
    driver.temperature = 40;
    assert(reader.read() == 40); // Sensor can recover after all ranges failed.
}
''')

    def test_ota_pauses_capture_and_restores_only_after_failure(self):
        code = r'''
#include <cassert>
bool otaInProgress = false, otaRestoreRtsp = false, rtspServerEnabled = true;
bool overheatLatched = false, scheduleAllowed = true, restartOk = true;
bool captureRunning = true;
unsigned overheatConsecutiveSamples = 2;
int clientsStopped = 0, pauses = 0, restarts = 0, starts = 0;
void stopAllRtspClients(const char*) { ++clientsStopped; }
void pauseAudioCapture() { captureRunning = false; ++pauses; }
void simplePrintln(const char*) {}
bool isStreamScheduleAllowedNow() { return scheduleAllowed; }
bool setup_i2s_driver() { ++restarts; captureRunning = restartOk; return restartOk; }
struct {
    void begin() { ++starts; }
    void setNoDelay(bool) {}
    void stop() {}
} rtspServer;
'''
        code += function('void beginOtaUpdate()') + '\n'
        code += function('void endOtaUpdate(bool installed)') + '\n'
        code += r'''
int main() {
    beginOtaUpdate();
    assert(otaInProgress && !captureRunning && !rtspServerEnabled);
    assert(overheatConsecutiveSamples == 0 && pauses == 1 && clientsStopped == 1);
    beginOtaUpdate(); // Repeated multipart callbacks must not overwrite saved state.
    assert(pauses == 1);
    endOtaUpdate(true);
    assert(otaInProgress && !captureRunning && !rtspServerEnabled && restarts == 0);
    endOtaUpdate(false); // A failed/aborted transfer restores the previous service.
    assert(!otaInProgress && captureRunning && rtspServerEnabled && starts == 1);
    endOtaUpdate(false);
    assert(restarts == 1);
    rtspServerEnabled = false;
    beginOtaUpdate(); endOtaUpdate(false);
    assert(captureRunning && !rtspServerEnabled && starts == 1);
    rtspServerEnabled = true; scheduleAllowed = false;
    beginOtaUpdate(); endOtaUpdate(false);
    assert(captureRunning && !rtspServerEnabled && starts == 1);
    scheduleAllowed = true; rtspServerEnabled = true;
    beginOtaUpdate(); overheatLatched = true; endOtaUpdate(false);
    assert(!captureRunning && !rtspServerEnabled); // Never bypass a thermal latch.
    overheatLatched = false; rtspServerEnabled = true; restartOk = false;
    beginOtaUpdate(); endOtaUpdate(false);
    assert(!captureRunning && !rtspServerEnabled);
}
'''
        self.run_cpp(code)

    def test_capture_stops_before_clocks_and_pdm_memory_are_released(self):
        code = r'''
#include <cassert>
bool producerRunning = true, clockRunning = true, pdmAllocated = true;
bool audioDmaCallbackAvailable = true;
int channel = 1;
int* i2s_rx_handle = &channel;
void stopAudioProducer() { producerRunning = false; }
void i2s_channel_disable(int*) { assert(!producerRunning); clockRunning = false; }
void i2s_del_channel(int*) { assert(!producerRunning && !clockRunning); }
void releasePdmCapture() { assert(!producerRunning && !i2s_rx_handle); pdmAllocated = false; }
'''
        code += function('void pauseAudioCapture()') + '\n'
        code += r'''
int main() {
    pauseAudioCapture();
    assert(!producerRunning && !clockRunning && !pdmAllocated && !i2s_rx_handle);
    assert(!audioDmaCallbackAvailable);
    pauseAudioCapture(); // Repeated shutdown remains safe after resources are gone.
}
'''
        self.run_cpp(code)

    def test_every_update_transport_uses_pause_and_failure_recovery(self):
        webui = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()
        self.assertIn('ArduinoOTA.onStart([] { beginOtaUpdate(); });', SOURCE)
        self.assertIn('ArduinoOTA.onError([](ota_error_t) { endOtaUpdate(false); });', SOURCE)
        pull = webui[webui.index('static bool installOtaFromUrl('):webui.index('static void httpOtaPage(')]
        self.assertLess(pull.index('beginOtaUpdate();'), pull.index('streamUpdate('))
        self.assertIn('endOtaUpdate(ok);', pull)
        upload = webui[webui.index('static void httpOtaUploadChunk('):webui.index('static void httpStatus(')]
        self.assertLess(upload.index('beginOtaUpdate();'), upload.index('Update.begin('))
        self.assertIn('endOtaUpdate(otaUploadOk);', upload)
        self.assertIn('endOtaUpdate(false);', upload[upload.index('UPLOAD_FILE_ABORTED'):])
        setup = function('bool setup_i2s_driver()')
        self.assertLess(setup.index('if (overheatLatched || otaInProgress) return true;'),
                        setup.index('i2s_new_channel('))
