"""Exercise thermal policy and the actual firmware shutdown/recovery functions."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()


def function(signature):
    start = SOURCE.index(signature + " {")
    brace = SOURCE.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


class ThermalRecoveryTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text('namespace recording { inline bool enabled=false,active=false,pending=false; inline bool mode(){return enabled;} inline bool resumePending(){return pending;} inline void cancelResume(){pending=false;} inline bool pauseForCooling(){pending=active;active=false;return pending;} }\n' + code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_policy_boundaries_persistence_and_timing(self):
        self.compile_run((ROOT / 'tests/thermal_recovery_test.cpp').read_text())

    def test_actual_shutdown_recovery_acknowledgement_and_cancel(self):
        code = r'''
#include "ThermalRecovery.h"
#include <cassert>
#include <string>
#include <algorithm>
using std::max;
struct String : std::string {
    using std::string::string;
    String(const std::string& s) : std::string(s) {}
    String(float x, int) : std::string(std::to_string(x)) {}
};
uint32_t nowMs = 1000, bootTime = 0;
uint32_t millis() { return nowMs; }
int64_t esp_timer_get_time() { return int64_t(nowMs) * 1000; }
uint32_t thermalEpochNow() { return 1800000000 + nowMs / 1000; }
String formatUptime(unsigned long) { return "uptime"; }
float temperature = 40, lastTemperatureC = 40, maxTemperature = 40;
float overheatShutdownC = 80, overheatTripTemp = 0;
float readChipTemperature() { return temperature; }
bool overheatLatched = false, overheatLockoutActive = false, overheatSensorFault = false;
bool overheatProtectionEnabled = true, overheatAutoResumeEnabled = true;
bool otaInProgress = false, captureRunning = true, restartOk = true;
bool &audioProducerRunning=captureRunning;
int recordingResumes=0;
bool resumeRecordingAfterCooling(){++recordingResumes;recording::active=true;recording::pending=false;return true;}
int capturePauses = 0;
void pauseAudioCapture() { captureRunning = false; ++capturePauses; }
bool setup_i2s_driver() { captureRunning = restartOk; return restartOk; }
bool lastTemperatureValid = true, rtspServerEnabled = true, scheduleAllowed = true;
uint8_t overheatConsecutiveSamples = 0;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason, overheatLastTimestamp;
const uint8_t OVERHEAT_CONFIRM_SAMPLES = 3;
const float OVERHEAT_EMERGENCY_MARGIN_C = 10, OVERHEAT_MIN_LIMIT_C = 30;
ThermalRecovery thermalRecovery;
ThermalRecoveryState flash;
int starts = 0, stops = 0, writes = 0;
struct {
    void begin(const char*, bool) {}
    void end() {}
    void putString(const char*, const String&) {}
    void putFloat(const char*, float) {}
    void putBool(const char*, bool) {}
    void putBytes(const char*, const void* data, size_t) {
        flash = *static_cast<const ThermalRecoveryState*>(data); ++writes;
    }
} audioPrefs;
struct {
    void begin() { ++starts; }
    void setNoDelay(bool) {}
    void stop() { ++stops; }
} rtspServer;
void simplePrintln(const String&) {}
void stopAllRtspClients(const char*) {}
bool mqttPublishState(bool) { return true; }
bool isStreamScheduleAllowedNow() { return scheduleAllowed; }
'''
        for signature in ['static bool isTemperatureValid(float temp)',
                          'static void persistOverheatNote()', 'void cancelThermalResume()',
                          'bool acknowledgeThermalShutdown()', 'void recordOverheatTrip(float temp)',
                          'void checkTemperature()']:
            code += function(signature) + '\n'
        code += r'''
void sample(float temp) { nowMs += 5000; temperature = temp; checkTemperature(); }
void cooldown() { for (unsigned i = 0; i < 61; ++i) sample(70); }
void trip() { sample(81); sample(81); sample(81); }
int main() {
    sample(NAN); sample(INFINITY); sample(131);
    assert(!overheatLatched && stops == 0); // Invalid sensor values cannot trip protection.
    sample(40);
    sample(81); sample(40); // Isolated spike must not stop RTSP.
    assert(!overheatLatched && stops == 0);
    trip();
    assert(overheatLatched && !rtspServerEnabled && stops == 1);
    assert(!captureRunning && capturePauses == 1);
    assert(flash.latched && flash.count == 1);
    assert(!acknowledgeThermalShutdown());
    sample(81); sample(81); // Still hot while latched: one event only.
    assert(thermalRecovery.state.count == 1);
    auto reason = overheatLastReason;
    sample(NAN);
    assert(overheatSensorFault && overheatLastReason == reason);
    cooldown();
    assert(!overheatLatched && rtspServerEnabled && starts == 1);
    assert(captureRunning);
    assert(flash.notice && flash.resumed && flash.count == 1);
    assert(acknowledgeThermalShutdown());
    assert(!flash.notice && flash.count == 1 && overheatLastReason == reason);
    trip(); cooldown();
    assert(starts == 2 && flash.count == 2 && !overheatLatched);
    trip(); cooldown();
    assert(overheatLatched && flash.manualBlocked && flash.count == 3 && starts == 2);
    // Persist/reload real saved state: blocks remain, cooling starts anew.
    thermalRecovery.restore(flash, true, 0);
    assert(!acknowledgeThermalShutdown());
    scheduleAllowed = false;
    cooldown();
    assert(acknowledgeThermalShutdown());
    assert(!overheatLatched && !rtspServerEnabled && starts == 2 && flash.count == 0);
    // Manual stop while waiting cancels automatic restoration.
    scheduleAllowed = true; rtspServerEnabled = true;
    trip(); cancelThermalResume(); cooldown();
    assert(overheatLatched && !flash.resumePending && starts == 2);
    assert(acknowledgeThermalShutdown() && starts == 3);
    // Emergency heating after an ordinary trip escalates that same event.
    trip(); sample(90); cooldown();
    assert(overheatLatched && flash.emergency && flash.count == 1 && starts == 3);
    assert(acknowledgeThermalShutdown());
    // Emergency first sample also shuts down immediately.
    sample(90);
    assert(overheatLatched && flash.emergency && !rtspServerEnabled);
    cooldown(); assert(acknowledgeThermalShutdown());
    // Schedule closing after a normal trip defers automatic restoration.
    trip(); scheduleAllowed = false; cooldown();
    assert(overheatLatched && starts == 5);
    int before = writes;
    sample(70); sample(70);
    assert(writes == before); // No periodic flash writes while cooling.
    scheduleAllowed = true; sample(70);
    assert(!overheatLatched && starts == 6 && flash.notice);
    // Failed capture restart must retain the persisted shutdown and retry budget.
    trip(); restartOk = false; cooldown();
    assert(overheatLatched && flash.latched && !rtspServerEnabled);
    restartOk = true; cooldown();
    assert(!overheatLatched && captureRunning && rtspServerEnabled);
    // Manual recovery also keeps the event if capture cannot restart.
    sample(90); restartOk = false; cooldown();
    assert(!acknowledgeThermalShutdown() && overheatLatched && flash.latched);
    restartOk = true; cooldown();
    assert(acknowledgeThermalShutdown() && captureRunning && !overheatLatched);
    sample(90); sample(NAN);
    assert(overheatLatched && overheatSensorFault && !acknowledgeThermalShutdown());
    overheatProtectionEnabled = false;
    sample(NAN);
    assert(overheatLatched && !captureRunning); // OFF itself does not clear the latch.
    otaInProgress = true;
    assert(!acknowledgeThermalShutdown()); // An active update still owns capture.
    otaInProgress = false;
    restartOk = false;
    assert(!acknowledgeThermalShutdown() && flash.latched && !captureRunning);
    restartOk = true;
    auto lastReason = overheatLastReason;
    assert(acknowledgeThermalShutdown() && captureRunning && rtspServerEnabled);
    assert(!flash.latched && !flash.notice && flash.count == 0);
    assert(overheatLastReason == lastReason); // Keep diagnosis after manual override.
    overheatProtectionEnabled = true;
    sample(90);
    assert(overheatLatched && !acknowledgeThermalShutdown());
    overheatProtectionEnabled = false; scheduleAllowed = false;
    assert(acknowledgeThermalShutdown() && !rtspServerEnabled);
    // Recording gets the same trip/cooldown/retry policy without an RTSP server
    // and without being gated by a Streaming schedule.
    thermalRecovery = ThermalRecovery{}; overheatLatched=false;
    overheatProtectionEnabled=overheatAutoResumeEnabled=true;
    recording::enabled=recording::active=true;captureRunning=true;rtspServerEnabled=false;
    scheduleAllowed=false;trip();
    assert(overheatLatched&&recording::pending&&!captureRunning);
    cooldown();assert(!overheatLatched&&recordingResumes==1&&recording::active&&!rtspServerEnabled);
    trip();cancelThermalResume();cooldown();
    assert(overheatLatched&&!recording::pending&&recordingResumes==1);
    assert(acknowledgeThermalShutdown()&&recordingResumes==1&&!rtspServerEnabled);
    recording::active=true;trip();cooldown();
    assert(recordingResumes==2&&!overheatLatched);
    trip();cooldown();assert(!overheatLatched&&recordingResumes==3);
    trip();cooldown();assert(overheatLatched&&flash.manualBlocked&&recordingResumes==3);
    assert(acknowledgeThermalShutdown()&&recordingResumes==4);
    overheatAutoResumeEnabled=false;trip();cooldown();
    assert(overheatLatched&&recordingResumes==4);
    assert(acknowledgeThermalShutdown()&&recordingResumes==5);
}
'''
        self.compile_run(code)
