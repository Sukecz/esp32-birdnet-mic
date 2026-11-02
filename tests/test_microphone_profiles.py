from webui_source import webui_source
from firmware_source import firmware_source
from validation_fixture import uint_parser
"""Profiles, actual capture transaction, CPU limits and authenticated group save."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKETCH = firmware_source(ROOT)
WEB = webui_source(ROOT)


def function(source, signature):
    start = source.index(signature + ' {')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class MicrophoneProfilesTests(unittest.TestCase):
    def native(self, source):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_all_board_microphone_profiles_and_unknown_target(self):
        self.native(r'''
#include "MicrophoneProfiles.h"
#include <cassert>
int main(){
 for(int board=micprofiles::C3;board<=micprofiles::C6;++board){
  for(int format=0;format<3;++format){
   micprofiles::Settings p;
   assert(micprofiles::recommended(micprofiles::Board(board),format,p));
   assert(p.rate==48000 && p.buffer==512 && p.highpass && p.cutoff==500 && p.shift==12);
   assert(p.gain==(format==2?24.0f:1.5f));
   assert(p.cpu==((format==2&&board!=micprofiles::S3)?160:0));
  }
 }
 micprofiles::Settings p={123,2,3,4,false,5,6};
 assert(!micprofiles::recommended(micprofiles::Unknown,0,p)&&p.rate==123);
 assert(!micprofiles::recommended(micprofiles::C5,3,p)&&p.rate==123);
}
''')

    def test_actual_transaction_and_full_rollback(self):
        self.native(r'''
#include "MicrophoneProfiles.h"
#include <cassert>
#include <cstdint>
#include <vector>
constexpr uint8_t MIC_FORMAT_PDM=2;
uint8_t micFormat=0,cpuFrequencyMhz=80,i2sShiftBits=11;
uint32_t currentSampleRate=96000,minAcceptableRate=33;
float currentGainFactor=2.75f;
uint16_t currentBufferSize=2048,highpassCutoffHz=1300;
bool highpassEnabled=false,autoThresholdEnabled=true,supported=true,clockAllowed=true,stopped=false;
int stops=0,restarts=0,clockCalls=0,keepCalls=0;
std::vector<bool> restartResults;
micprofiles::Board board=micprofiles::C6;
bool getRecommendedMicSettings(uint8_t f,micprofiles::Settings& p){return micprofiles::recommended(board,f,p);}
bool pdmMicSupported(){return supported;}
bool micBoardFormatAllowed(uint8_t){return true;}
void stopAudioProducer(){stopped=true;++stops;}
uint32_t computeRecommendedMinRate(){return currentSampleRate/currentBufferSize/2;}
bool setCpuFrequencyMhz(uint32_t){assert(stopped);++clockCalls;return clockAllowed;}
bool restartI2S(){assert(stopped);++restarts;bool r=restartResults.front();restartResults.erase(restartResults.begin());return r;}
bool applyMicFormatConfig(uint8_t f){++keepCalls;micFormat=f;return true;}
void simplePrintln(const char*){}
''' + function(SKETCH, 'bool applyMicProfileConfig(uint8_t newFormat, bool recommended)') + r'''
void oldState(){assert(micFormat==0&&currentSampleRate==96000&&currentGainFactor==2.75f&&currentBufferSize==2048&&i2sShiftBits==11&&!highpassEnabled&&highpassCutoffHz==1300&&cpuFrequencyMhz==80&&minAcceptableRate==33);}
int main(){
 assert(!applyMicProfileConfig(3,true)&&stops==0);
 supported=false;assert(!applyMicProfileConfig(2,true)&&stops==0);supported=true;
 board=micprofiles::Unknown;assert(!applyMicProfileConfig(0,true)&&stops==0);board=micprofiles::C6;
 restartResults={false,true};assert(!applyMicProfileConfig(2,true));oldState();assert(restarts==2&&clockCalls==2);
 clockAllowed=false;restartResults={true};assert(!applyMicProfileConfig(2,true));oldState();clockAllowed=true;
 restartResults={true};int count=restarts;assert(applyMicProfileConfig(2,true));
 assert(restarts==count+1&&micFormat==2&&currentSampleRate==48000&&currentGainFactor==24&&currentBufferSize==512&&i2sShiftBits==11&&highpassEnabled&&highpassCutoffHz==500&&cpuFrequencyMhz==160&&minAcceptableRate==46);
 restartResults={true};assert(applyMicProfileConfig(1,true));assert(micFormat==1&&currentGainFactor==1.5f&&i2sShiftBits==12);
 // Reapplying a profile to the same microphone must reset custom values too.
 currentGainFactor=9;restartResults={true};assert(applyMicProfileConfig(1,true));assert(currentGainFactor==1.5f);
 currentGainFactor=7;autoThresholdEnabled=false;minAcceptableRate=77;
 restartResults={true};assert(applyMicProfileConfig(0,true));assert(minAcceptableRate==77);
 currentGainFactor=7;assert(applyMicProfileConfig(2,false));assert(keepCalls==1&&currentGainFactor==7);
 board=micprofiles::S3;cpuFrequencyMhz=240;restartResults={true};assert(applyMicProfileConfig(2,true));assert(cpuFrequencyMhz==240);
}
''')

    def test_actual_board_cpu_limits(self):
        code = (function(SKETCH, 'uint16_t maxSupportedCpuFrequencyMhz()') + '\n' +
                function(SKETCH, 'bool cpuFrequencySupported(uint32_t mhz)'))
        for board in ('ESP32C3', 'ESP32S3', 'ESP32C5', 'ESP32C6'):
            with self.subTest(board=board):
                self.native('#define CONFIG_IDF_TARGET_' + board + ' 1\n#include <cstdint>\n#include <cassert>\n' + code + '\nint main(){assert(cpuFrequencySupported(40)&&cpuFrequencySupported(80)&&cpuFrequencySupported(160));assert(!cpuFrequencySupported(120)&&!cpuFrequencySupported(320)&&!cpuFrequencySupported(0));assert(cpuFrequencySupported(240)==' + ('true' if board in ('ESP32S3','ESP32C5') else 'false') + ');}\n')

    def test_actual_cpu_save_and_boot_repair(self):
        cpu = function(SKETCH, 'uint16_t maxSupportedCpuFrequencyMhz()') + '\n' + function(SKETCH, 'bool cpuFrequencySupported(uint32_t mhz)')
        handler = function(WEB, '    else if (key == "cpu_freq")').replace('else if (key == "cpu_freq")', 'if (true)', 1)
        repair = function(SKETCH, '    if (!cpuFrequencySupported(cpuFrequencyMhz))')
        for board in ('ESP32C3', 'ESP32S3', 'ESP32C5', 'ESP32C6'):
            with self.subTest(board=board):
                self.native('#define CONFIG_IDF_TARGET_' + board + ' 1\n' + r'''
#include <cstdint>
#include <cassert>
uint8_t cpuFrequencyMhz=160;
uint32_t input=240;
int saves=0,clockCalls=0;
bool applied=false,handled=false,clockOk=true,settingsRepaired=false;
bool argToUInt(uint32_t& v){v=input;return true;}
bool setCpuFrequencyMhz(uint32_t){++clockCalls;return clockOk;}
void saveAudioSettings(){++saves;}
''' + cpu + '\nvoid save(){' + handler + '}\nvoid boot(){' + repair + '}' + r'''
int main(){
 save();
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C5)
 assert(applied&&saves==1&&cpuFrequencyMhz==240&&clockCalls==1);
 boot();assert(cpuFrequencyMhz==240&&!settingsRepaired);
#else
 assert(!applied&&!saves&&cpuFrequencyMhz==160&&!clockCalls);
 cpuFrequencyMhz=240;boot();assert(cpuFrequencyMhz==160&&settingsRepaired);
#endif
 cpuFrequencyMhz=160;saves=0;applied=false;clockOk=false;input=80;
 save();assert(!applied&&!saves&&cpuFrequencyMhz==160);
 input=120;clockOk=true;save();assert(!applied&&!saves&&cpuFrequencyMhz==160);
}
''')

    def test_actual_authenticated_microphone_save(self):
        parse = uint_parser()
        self.native(r'''
#include <string>
#include <map>
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
struct String:std::string{using std::string::string;};
#define F(s) s
struct {std::map<std::string,String> args;String arg(const char* k){return args[k];}} web;
bool authorized=true,applyOk=true;int calls=0,saves=0;uint8_t lastFormat=99;bool lastRecommended=false;std::string reply;
bool requireMutationAuth(){return authorized;}
void apiSendJSON(const char* s){reply=s;}
void saveAudioSettings(){++saves;}
bool applyMicProfileConfig(uint8_t f,bool r){++calls;lastFormat=f;lastRecommended=r;return applyOk;}
''' + parse + function(WEB, 'static void httpSaveMicProfile()') + r'''
int main(){
 web.args={{"format","2"},{"settings","recommended"}};authorized=false;httpSaveMicProfile();assert(!calls&&!saves);authorized=true;
 for(const char* bad:{"3","-1","256","2x","","9999999999999999999999"}){web.args["format"]=bad;httpSaveMicProfile();assert(!calls&&!saves);}
 web.args["format"]="2";web.args["settings"]="invalid";httpSaveMicProfile();assert(!calls&&!saves);
 web.args["settings"]="recommended";applyOk=false;httpSaveMicProfile();assert(calls==1&&!saves&&reply.find("i2s_restart_failed")!=std::string::npos);
 applyOk=true;httpSaveMicProfile();assert(saves==1&&lastFormat==2&&lastRecommended);
 web.args["format"]="0";web.args["settings"]="keep";httpSaveMicProfile();assert(saves==2&&lastFormat==0&&!lastRecommended);
}
''')


if __name__ == '__main__':
    unittest.main()
