"""S31 storage capability separation and fail-closed codec initialization."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class S31BoardTests(unittest.TestCase):
    def test_mode_is_available_without_an_sd_route(self):
        self.native('''#include "RecordingPins.h"
#include <cassert>
int main(){assert(recordingpins::modeSupported());assert(!recordingpins::resolve().supported);}
''')

    def test_codec_errors_never_report_capture_ready(self):
        wire = '''#pragma once
#include <cstdint>
#include <cassert>
#include <vector>
constexpr int LOW=0,OUTPUT=1;
inline int pa=-1,writes=0,fail=-1;inline bool bus=true;
inline std::vector<uint8_t> values;
inline void digitalWrite(int p,int v){assert(p==57);pa=v;}
inline void pinMode(int p,int m){assert(p==57&&m==OUTPUT);}
inline void delay(int ms){assert(ms==20);}
struct Bus {
 bool begin(int sda,int scl,int speed){assert(sda==51&&scl==50&&speed==400000);return bus;}
 void beginTransmission(int address){assert(address==0x18);values.clear();}
 void write(uint8_t v){values.push_back(v);}
 int endTransmission(){assert(values.size()==2);return writes++==fail?1:0;}
};inline Bus Wire;
'''
        self.native('''#include "S31Codec.h"
int main(){
 assert(!s31codec::begin(96000));assert(writes==0);
 assert(s31codec::begin(48000));assert(pa==LOW);const int total=writes;
 for(int i=0;i<total;i++){writes=0;fail=i;assert(!s31codec::begin(48000));assert(writes==i+1);}
 fail=-1;writes=0;bus=false;assert(!s31codec::begin(48000));assert(writes==0);
 bus=true;assert(s31codec::begin(16000));assert(pa==LOW);
}
''', wire)

    def native(self, code, wire=None):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text('#pragma once\n#include <cstdint>\n')
            if wire:
                (tmp / 'Wire.h').write_text(wire)
            (tmp / 'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-DCONFIG_IDF_TARGET_ESP32S31=1', '-I', str(tmp),
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(tmp / 'test.cpp'),
                            '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)
