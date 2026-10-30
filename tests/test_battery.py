"""Production ADC state machine: opt-in, staged reads, gating, invalid/stale values."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_device_setup import STUBS

ROOT = Path(__file__).resolve().parents[1]
ARDUINO = STUBS['Arduino.h'].replace(
    ' String(int v):std::string(std::to_string(v)){}',
    ' String(int v):std::string(std::to_string(v)){}\n'
    ' String(float v,int places){char b[32];snprintf(b,sizeof(b),"%.*f",places,double(v));assign(b);}') + r'''
constexpr int A0=TEST_ADC, BAT_VOLT_PIN=6, BAT_VOLT_PIN_EN=26;
constexpr int INPUT=0,OUTPUT=1,HIGH=1,LOW=0,ADC_11db=3;
inline int reads=0,rawMv=1950,lastGate=-1,configured=-1;
inline void pinMode(int,int){}
inline void digitalWrite(int pin,int value){assert(pin==26);lastGate=value;}
inline void analogSetPinAttenuation(int pin,int attenuation){configured=pin;assert(attenuation==ADC_11db);}
inline uint32_t analogReadMilliVolts(int pin){assert(pin==configured);++reads;return rawMv;}
'''
HARNESS = r'''
#include "Battery.h"
#include <Preferences.h>
void tick(uint32_t duration){clockMs+=duration;int before=reads;battery::loop(true,0);assert(reads-before<=1);}
void sample(){tick(1);for(int i=0;i<80;++i)tick(2);}
void has(const char* text){assert(battery::statusJson(true,0).find(text)!=std::string::npos);}
int main(int argc,char** argv){
 if(argc>1){
  const uint32_t saved=argv[1][0]=='v'?10500:20000;
  const auto* bytes=reinterpret_cast<const uint8_t*>(&saved);
  nvs["battery/scale"]={bytes,bytes+sizeof(saved)};
  sample();has(argv[1][0]=='v'?"\"calibration_percent\":5.00":"\"calibration_percent\":0.00");return 0;
 }
 const auto pins=battery::pins();assert(pins.supported&&pins.adc==TEST_ADC);
 assert(battery::pins(2).supported==!bool(TEST_S3));
 battery::loop(false,0);assert(reads==0&&configured==-1);
 assert(battery::statusJson(false,0).find("\"voltage\":null")!=std::string::npos);
 clockMs=0xFFFFFFF0;sample(); // divider settling and sample cadence across millis wrap.
 assert(battery::reading(true,0).valid&&battery::reading(true,0).millivolts==3900);
 assert(!battery::reading(false,0).valid);
 assert(reads==16);has("\"voltage\":3.90");has("\"percent\":60");
 if(pins.enable>=0)assert(lastGate==LOW);
 for(int i=0;i<100;++i)tick(5);
 assert(reads==16); // no repeat conversion before interval.
 clockMs+=10000;rawMv=2050;sample();has("\"voltage\":3.95");
 assert(battery::reading(true,0).millivolts==4100);
 clockMs+=10000;rawMv=0;sample();has("\"valid\":false");has("\"percent\":null");
 clockMs+=10000;rawMv=2400;sample();has("\"voltage\":null"); // outside supported pack range.
 clockMs+=10000;rawMv=1950;sample();has("\"voltage\":3.90"); // recovers without old invalid samples.
 clockMs+=30001;has("\"valid\":false");assert(!battery::reading(true,0).valid);
 clockMs+=10000;sample();
 // Single-point calibration acts on raw voltage and survives reloading.
 assert(battery::calibrate(4000,0));has("\"voltage\":4.00");assert(battery::reading(true,0).millivolts==4000);has("\"percent\":75");
 auto saved=nvs["battery/scale"];assert(!saved.empty());
 assert(battery::calibrate(4000,0));assert(nvs["battery/scale"]==saved);
 assert(!battery::calibrate(4300,0));assert(nvs["battery/scale"]==saved);
 failWrite=true;assert(!battery::resetCalibration());has("\"voltage\":4.00");failWrite=false;
 assert(battery::resetCalibration());has("\"voltage\":3.90");
 clockMs+=30001;assert(!battery::calibrate(4000,0));
 int before=reads;battery::loop(false,0);assert(reads==before);
 rawMv=1800;sample();assert(reads==before+16);has("\"voltage\":3.60");
 if(TEST_S3){battery::loop(true,2);assert(reads==before+16);assert(battery::statusJson(true,2).find("\"supported\":false")!=std::string::npos);}
 int previous=0;for(int mv=0;mv<=5000;++mv){int pct=battery::percent(mv);assert(pct>=previous&&pct<=100);previous=pct;}
 assert(battery::percent(3300)==0&&battery::percent(4200)==100);
}
'''


class BatteryTests(unittest.TestCase):
    def test_production_monitor_all_boards(self):
        for board, adc in [('C3', 2), ('S3', 1), ('C5', 6), ('C6', 0)]:
            with self.subTest(board=board), tempfile.TemporaryDirectory() as directory:
                tmp = Path(directory)
                (tmp / 'Arduino.h').write_text(ARDUINO)
                (tmp / 'Preferences.h').write_text(STUBS['Preferences.h'])
                (tmp / 'test.cpp').write_text(HARNESS)
                subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                                '-DARDUINO_XIAO_ESP32' + board, '-DTEST_ADC=' + str(adc),
                                '-DTEST_S3=' + str(int(board == 'S3')),
                                '-I', str(tmp), '-I', str(ROOT / 'esp32-birdnet-mic'),
                                str(tmp / 'test.cpp'), str(ROOT / 'esp32-birdnet-mic/Battery.cpp'),
                                '-o', str(tmp / 'test')], check=True)
                for args in ([], ['valid-saved'], ['invalid-saved']):
                    subprocess.run([str(tmp / 'test')]+args, check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
