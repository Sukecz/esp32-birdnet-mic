"""Shared S3 image: routing, atomic switching, saved selection and API validation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
WEB = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()


def function(source, signature):
    start = source.index(signature + ' {')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class S3BoardsTests(unittest.TestCase):
    def native(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
            cpp.write_text('namespace recording { inline bool active=false,working=false; inline int checks=0,lastVariant=-1; inline bool mode(){return active;} inline bool busy(){return working;} inline bool checkCard(bool supported,unsigned char variant){lastVariant=variant;++checks;return supported;} }\nbool recordingHardwareSupported(){return true;}\n'+code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_actual_switch_transaction_routing_and_recovery(self):
        helpers = '\n'.join(function(SKETCH, sig) for sig in (
            'bool builtinMicSelected()', 'bool micBoardFormatAllowed(uint8_t format)',
            'void updateS3BoardIdentity()', 's3boards::Config getS3BoardConfig()',
            'bool restoreS3BoardConfig(const s3boards::Config& saved)', 'bool applyS3BoardConfig(uint8_t variant)',
            'bool applyMicFormatConfig(uint8_t newFormat)'))
        self.native(r'''
#include "S3Boards.h"
#include <cassert>
#include <cstring>
#include <vector>
uint8_t s3BoardVariant=0,micFormat=0;
constexpr uint8_t MIC_FORMAT_PDM=2;
constexpr uint32_t PDM_SAMPLE_RATE=48000;
uint32_t currentSampleRate=96000,minAcceptableRate=33;
uint16_t highpassCutoffHz=30000;
bool autoThresholdEnabled=true,supported=true,stopped=false;
const char* FW_BOARD_ID_STR="xiao-esp32s3";
const char* FW_BOARD_NAME_STR="XIAO ESP32-S3";
int stops=0,restarts=0;
std::vector<bool> results;
bool s3BoardSelectionSupported(){return supported;}
void stopAudioProducer(){stopped=true;++stops;}
bool restartI2S(){assert(stopped);++restarts;bool ok=results.front();results.erase(results.begin());return ok;}
uint16_t normalizeHighpassCutoff(uint32_t rate,uint16_t cutoff){return cutoff<rate/2?cutoff:rate/2-1;}
uint32_t computeRecommendedMinRate(){return currentSampleRate/1024;}
bool pdmMicSupported(){return true;}
void simplePrintln(const char*){}
''' + helpers + r'''
int main(){
 assert(!s3boards::valid(3)&&!s3boards::valid(255));
 assert(s3boards::clockPin(0,4)==4&&s3boards::dataPin(0,3)==3);
 assert(s3boards::clockPin(1,4)==42&&s3boards::dataPin(1,3)==41);
 assert(s3boards::clockPin(2,4)==39&&s3boards::dataPin(2,3)==38);
 assert(micBoardFormatAllowed(0)&&micBoardFormatAllowed(1)&&micBoardFormatAllowed(2));
 assert(!applyS3BoardConfig(3)&&!stops);
 supported=false;assert(!applyS3BoardConfig(1)&&!stops);supported=true;
 assert(applyS3BoardConfig(0)&&!stops);
 results={false,true};assert(!applyS3BoardConfig(1));
 assert(s3BoardVariant==0&&micFormat==0&&currentSampleRate==96000&&highpassCutoffHz==30000&&minAcceptableRate==33);
 assert(!strcmp(FW_BOARD_ID_STR,"xiao-esp32s3")&&restarts==2);
 recording::working=true;assert(!applyS3BoardConfig(1)&&s3BoardVariant==0);recording::working=false;
 const auto original=getS3BoardConfig();
 recording::active=true;
 results={true};assert(applyS3BoardConfig(1));
 assert(recording::checks==0); // Hardware switching never starts an SD operation before commit.
 assert(!applyS3BoardConfig(2)&&recording::checks==0);
 recording::active=false;
 assert(s3BoardVariant==1&&micFormat==2&&currentSampleRate==48000&&highpassCutoffHz==23999);
 assert(!strcmp(FW_BOARD_ID_STR,"xiao-esp32s3-sense"));
 assert(micBoardFormatAllowed(0)&&micBoardFormatAllowed(1)&&micBoardFormatAllowed(2));
 micFormat=0;assert(!builtinMicSelected());micFormat=2;assert(builtinMicSelected());
 int count=restarts;results={true};assert(applyS3BoardConfig(2)&&restarts==count+1);
 assert(!strcmp(FW_BOARD_NAME_STR,"M5Stack AtomS3U"));
 assert(!micBoardFormatAllowed(0)&&!micBoardFormatAllowed(1)&&micBoardFormatAllowed(2));
 results={false,true};assert(!applyS3BoardConfig(0)&&s3BoardVariant==2);
 assert(!strcmp(FW_BOARD_ID_STR,"m5stack-atoms3u"));
 autoThresholdEnabled=false;minAcceptableRate=77;results={true};assert(applyS3BoardConfig(0));
 assert(micFormat==2&&currentSampleRate==48000&&minAcceptableRate==77&&micBoardFormatAllowed(0));
 assert(!strcmp(FW_BOARD_ID_STR,"xiao-esp32s3"));
 recording::active=true;results={true};assert(applyS3BoardConfig(1));
 assert(recording::checks==0);
 int probes=recording::checks;results={false,true};assert(!applyS3BoardConfig(0));assert(recording::checks==probes&&s3BoardVariant==1);
 results={true};assert(applyS3BoardConfig(0));assert(recording::checks==0);
 recording::active=false;
 results={true};assert(applyS3BoardConfig(1));
 results={false,true};assert(!applyMicFormatConfig(0)&&micFormat==2&&s3BoardVariant==1);
 for(uint8_t format:{0,1,2}){
  results={true};assert(applyMicFormatConfig(format));
  assert(micFormat==format&&s3BoardVariant==1);
  assert(builtinMicSelected()==(format==2));
 }
 results={true};assert(applyS3BoardConfig(2));
 count=restarts;assert(!applyMicFormatConfig(0)&&!applyMicFormatConfig(1)&&restarts==count);
 results={true};assert(restoreS3BoardConfig(original));
 assert(s3BoardVariant==original.variant&&micFormat==original.format&&currentSampleRate==original.rate);
 assert(highpassCutoffHz==original.cutoff&&minAcceptableRate==original.minRate&&recording::checks==0);
 assert(!strcmp(FW_BOARD_ID_STR,"xiao-esp32s3"));
}
''')

    def test_boot_selection_and_cross_board_preferences(self):
        start = SKETCH.index('    s3BoardVariant = audioPrefs.getUChar(')
        end = SKETCH.index('    autoRecoveryEnabled =', start)
        for supported in (True, False):
            self.native(r'''
#include "S3Boards.h"
#include <cassert>
uint8_t s3BoardVariant=0,micFormat=0,saved=0;
bool settingsRepaired=false;
constexpr uint8_t MIC_FORMAT_PDM=2;
struct {uint8_t getUChar(const char*,uint8_t){return saved;}} audioPrefs;
void updateS3BoardIdentity(){}
bool s3BoardSelectionSupported(){return ''' + str(supported).lower() + ';}\n' + function(SKETCH, 'bool micBoardFormatAllowed(uint8_t format)') + '\nvoid boot(){' + SKETCH[start:end] + r'''
}
int main(){
 for(int fmt=0;fmt<3;++fmt)for(int value=0;value<256;++value){
  saved=value;micFormat=fmt;settingsRepaired=false;boot();
  if(s3BoardSelectionSupported()&&value<=2){assert(s3BoardVariant==value);assert(micFormat==(value==2?2:fmt));}
  else assert(s3BoardVariant==0&&micFormat==fmt);
 }
}
''')
        self.assertIn('putUChar("s3Board", s3BoardVariant)', SKETCH)

    def test_actual_endpoint_auth_strict_input_and_no_save_on_failure(self):
        parser = WEB[WEB.index('static bool parseUInt32Strict('):WEB.index('static bool parseInt32Strict(')]
        self.native(r'''
#include <string>
#include <map>
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
struct String:std::string{using std::string::string;};
#define F(s) s
struct {String value;String arg(const char*){return value;}} web;
bool auth=true,supported=true,applyOk=true;int calls=0,saves=0;uint8_t applied=99,s3BoardVariant=0;
bool requireMutationAuth(){return auth;}
bool s3BoardSelectionSupported(){return supported;}
bool applyS3BoardConfig(uint8_t v){++calls;applied=v;if(applyOk)s3BoardVariant=v;return applyOk;}
void saveAudioSettings(){++saves;}
void apiSendJSON(const char*){}
''' + parser + function(WEB, 'static void httpSaveS3Board()') + r'''
int main(){
 web.value="1";auth=false;httpSaveS3Board();assert(!calls&&!saves);auth=true;
 supported=false;httpSaveS3Board();assert(!calls&&!saves);supported=true;
 for(const char* bad:{"3","-1","256","1x","","999999999999999999999"}){web.value=bad;httpSaveS3Board();assert(!calls&&!saves);}
 web.value="2";applyOk=false;httpSaveS3Board();assert(calls==1&&!saves);
 applyOk=true;httpSaveS3Board();assert(calls==2&&saves==1&&applied==2);
 recording::active=true;web.value="1";httpSaveS3Board();
 assert(saves==2&&recording::checks==1&&recording::lastVariant==1);
}
''')


if __name__ == '__main__':
    unittest.main()
