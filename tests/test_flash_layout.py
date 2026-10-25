"""Actual coordinator, partition bytes, flash failures and migration interlocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / 'esp32-birdnet-mic'
GEN = Path.home() / '.arduino15/packages/esp32/hardware/esp32/3.3.12/tools/gen_esp32part.py'


class FlashLayoutTests(unittest.TestCase):
    def native(self, source=None, *, filename=None, defines=()):
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(filename) if filename else Path(temp) / 'test.cpp'
            if source is not None:
                cpp.write_text(source)
            binary = Path(temp) / 'test'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', *defines,
                            '-I', str(ROOT / 'tests/flash_layout_stubs'), '-I', str(SKETCH),
                            str(cpp), '-lcrypto', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_actual_coordinator_reboots_failures_recovery_and_preserved_settings(self):
        self.native(filename=ROOT / 'tests/flash_layout_runtime_test.cpp',
                    defines=['-DARDUINO_XIAO_ESP32C6'])

    def test_generated_tables_match_espressif_and_reject_custom_or_corrupt_data(self):
        with tempfile.TemporaryDirectory() as temp:
            self.native(r'''
#include "Platform.h"
#include "FlashLayoutTable.h"
#include <fstream>
static void digest(const uint8_t* bytes, uint32_t len, uint8_t* out) { MD5(bytes, len, out); }
int main() {
    using namespace flashtable;
    uint8_t old[kSectorSize], table[kSectorSize], scratch[kSectorSize];
    build(old, Default, digest); build(table, Enlarged, digest);
    assert(classify(old, scratch, digest) == Default);
    assert(classify(table, scratch, digest) == Enlarged);
    for (unsigned offset : {2u, 4u, 8u, 12u, 28u, 6*32u+16u, 0xD00u}) {
        uint8_t changed[kSectorSize]; memcpy(changed, old, sizeof(old));
        changed[offset] ^= 1; assert(classify(changed, scratch, digest) == Unknown);
    }
    std::ofstream("default.bin", std::ios::binary).write((char*)old, sizeof(old));
    std::ofstream("enlarged.bin", std::ios::binary).write((char*)table, sizeof(table));
}
'''.replace('"default.bin"', '"' + str(Path(temp) / 'default.bin') + '"')
   .replace('"enlarged.bin"', '"' + str(Path(temp) / 'enlarged.bin') + '"'))
            if GEN.exists():
                result = Path(temp) / 'expected.bin'
                subprocess.run(['python3', str(GEN), str(ROOT / 'tools/partitions/birdnet_4mb.csv'),
                                str(result)], check=True, capture_output=True)
                generated = (Path(temp) / 'enlarged.bin').read_bytes()
                expected = result.read_bytes().ljust(4096, b'\xff')
                self.assertEqual(generated, expected)
                csv = subprocess.check_output(['python3', str(GEN), str(Path(temp) / 'enlarged.bin')], text=True)
                self.assertIn('app1,app,ota_1,0x200000,1984K', csv)

    def test_s3_c5_and_generic_builds_have_no_runtime_migrator(self):
        self.native(r'''
#include "FlashLayout.cpp"
int main() {
    String error;
    flashlayout::begin(); flashlayout::loop();
    assert(!flashlayout::busy() && !flashlayout::start(error));
    assert(flashlayout::otaAllowed(error));
    assert(flashlayout::statusJson().find("unsupported") != std::string::npos);
}
''')

    def test_temperature_and_scheduled_actions_are_skipped_even_while_queued(self):
        source = (SKETCH / 'esp32-birdnet-mic.ino').read_text()
        prefix = source[source.index('void loop() {'):source.index('    if (millis() - lastTempCheck >', source.index('void loop() {'))]
        self.native(r'''
#include "Platform.h"
namespace recording { bool busy(){return false;} bool stop(){return true;} }
namespace flashlayout { bool pending=false; void loop(){} bool busy(){return pending;} }
int otaCalls=0, wifiCalls=0, webCalls=0, thermalCalls=0;
bool otaInProgress=false;
unsigned long scheduledRebootAt=0,lastWebuiHandleMs=0;
constexpr unsigned WEBUI_HANDLE_INTERVAL_MS=0;
struct {void handle(){++otaCalls;}} ArduinoOTA;
void wifiApRecoveryLoop(){++wifiCalls;}
void webui_handleClient(){++webCalls;}
''' + prefix + r'''
    ++thermalCalls; // All temperature/schedule/recovery work follows the guard.
}
int main(){
    fake::clock=100;
    flashlayout::pending=true; loop();
    assert(!otaCalls && !wifiCalls && !thermalCalls && webCalls==1);
    flashlayout::pending=false; otaInProgress=true; loop(); assert(!thermalCalls);
    otaInProgress=false; loop(); assert(thermalCalls==1);
}
''')

    def test_mutation_gate_preserves_csrf_and_blocks_competing_actions(self):
        source = (SKETCH / 'WebUI.cpp').read_text()
        gate = source[source.index('static bool requireMutationAuth('):source.index('// Static browser assets')]
        self.native(r'''
#include "Platform.h"
bool auth=false;
namespace recording { bool active=false; bool busy(){return active;} bool stop(){return true;} }
namespace flashlayout { bool active=false; bool busy(){return active;} }
bool hasMutationAuth(){return auth;}
struct { String route="/api/set"; String uri(){return route;} int code=0; void sendHeader(const char*,const char*){} void send(int c,const char*,const char*){code=c;} } web;
''' + gate + r'''
int main(){
    assert(!requireMutationAuth() && web.code==403);
    assert(!requireMutationAuth(true) && web.code==403);
    auth=true; assert(requireMutationAuth());
    flashlayout::active=true;
    assert(!requireMutationAuth() && web.code==409);
    assert(requireMutationAuth(true)); // Recovery retry only; still checks auth.
    flashlayout::active=false; recording::active=true;
    assert(!requireMutationAuth() && web.code==409);
    web.route="/api/recording/action"; assert(requireMutationAuth());
    web.route="/api/device/mode"; assert(requireMutationAuth());
}
''')
