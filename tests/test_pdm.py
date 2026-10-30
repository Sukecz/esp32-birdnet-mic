"""Native signal tests and actual firmware configuration/capture failure paths."""
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


class PdmTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 'test.cpp', pathlib.Path(temp) / 'test'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], timeout=45, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ui_modes_and_pending_edits(self):
        subprocess.run(['node', str(ROOT / 'tests/pdm_ui_test.js')], check=True, capture_output=True, timeout=10)

    def test_filter_frequency_response_bit_order_and_idle(self):
        self.compile_run((ROOT / 'tests/pdm_filter_test.cpp').read_text())

    def test_actual_transaction_and_rate_rejection(self):
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <algorithm>
constexpr uint8_t MIC_FORMAT_PHILIPS=0, MIC_FORMAT_MSB=1, MIC_FORMAT_PDM=2;
constexpr uint32_t PDM_SAMPLE_RATE=48000;
uint8_t micFormat=0, i2sShiftBits=12;
uint32_t currentSampleRate=96000, minAcceptableRate=30;
uint16_t highpassCutoffHz=30000, currentBufferSize=512;
float currentGainFactor=1.5f;
bool supported=true, autoThresholdEnabled=true, stopped=false;
std::vector<bool> restarts;
int stops=0, calls=0;
bool pdmMicSupported(){return supported;}
bool micBoardFormatAllowed(uint8_t){return true;}
void stopAudioProducer(){stopped=true; ++stops;}
bool restartI2S(){assert(stopped); ++calls; bool ok=restarts.front(); restarts.erase(restarts.begin()); return ok;}
void simplePrintln(const char*){}
uint16_t normalizeHighpassCutoff(uint32_t rate,uint16_t cutoff){return std::min<uint32_t>(rate/2-1,cutoff);}
uint32_t computeRecommendedMinRate(){return currentSampleRate/currentBufferSize/2;}
''' + function('bool applyMicFormatConfig(uint8_t newFormat)') + '\n' +
function('bool applyAudioConfig(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift)') + r'''
int main(){
 assert(!applyMicFormatConfig(255) && stops==0);
 supported=false; assert(!applyMicFormatConfig(2) && stops==0); supported=true;
 restarts={false,true};
 assert(!applyMicFormatConfig(2));
 assert(micFormat==0 && currentSampleRate==96000 && highpassCutoffHz==30000 && minAcceptableRate==30);
 assert(calls==2 && stops==1);
 restarts={true}; assert(applyMicFormatConfig(2));
 assert(micFormat==2 && currentSampleRate==48000 && highpassCutoffHz==23999 && i2sShiftBits==12);
 int before=stops;
 assert(!applyAudioConfig(96000,2.0f,256,10) && stops==before);
 restarts={true}; assert(applyAudioConfig(48000,2.0f,256,10));
 assert(micFormat==2 && currentBufferSize==256 && currentGainFactor==2.0f);
 restarts={false,true}; assert(!applyMicFormatConfig(1)); assert(micFormat==2 && currentSampleRate==48000);
 restarts={true}; assert(applyMicFormatConfig(1)); assert(micFormat==1 && currentSampleRate==48000);
}
''')

    def test_actual_capture_short_reads_duplicate_pairs_and_timeout(self):
        self.compile_run(r'''
#include "PdmCapture.h"
#include <cassert>
#include <atomic>
#include <vector>
#include <cstring>
#include <algorithm>
struct PdmReadBuffers {uint8_t bytes[2048]; uint16_t words[1025];};
PdmReadBuffers storage; PdmReadBuffers* pdmReadBuffers=&storage;
pdmsw::Decimator filter; pdmsw::Decimator* pdmDecimator=&filter;
pdmsw::WordFramer pdmFramer;
pdmsw::WordRateDetector pdmWordRate;
namespace recording {inline uint32_t faults=0;void captureFault(){++faults;}}
constexpr uint32_t PDM_SAMPLE_RATE=48000;
uint32_t getI2sDmaOverrunTotal(){return 0;}
std::atomic<uint8_t> pdmDetectedStride{0}, pdmDetectedOrder{1}, pdmStrideLogPending{0};
uint32_t pdmReadChunks=0, audioI2SErrorCount=0, audioProducerLastReadMs=0;
bool audioProducerStopRequested=false;
uint16_t currentBufferSize=512;
int32_t i2s_32bit_buffer[513]; int i2s_rx_handle=0;
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_ERR_TIMEOUT=1;
std::vector<uint8_t> dma; size_t dmaPos=0; bool shortReads=true, timedOut=false;
uint64_t clockUs=0; int arrivalStride=2;
uint64_t esp_timer_get_time(){return clockUs++;}
uint32_t millis(){return 123;}
int i2s_channel_read(int,void* out,size_t want,size_t* got,int){
 size_t limit=shortReads?size_t(1+dmaPos%127):want;
 *got=std::min({want,limit,dma.size()-dmaPos});
 memcpy(out,dma.data()+dmaPos,*got); dmaPos+=*got;
 clockUs+=(*got*1000000ULL)/(2*192000*arrivalStride)+1;
 return timedOut || *got==0?ESP_ERR_TIMEOUT:ESP_OK;
}
''' + function('static int readPdmSoftwareBlock(uint64_t& filterUs)') + r'''
void reset(const std::vector<uint16_t>& raw,int stride){
 dma.clear(); dmaPos=0; timedOut=false; audioProducerStopRequested=false; pdmReadChunks=0;
 pdmFramer.reset(); pdmFramer.stride=stride; pdmDetectedOrder=1;
 pdmWordRate.reset(); pdmWordRate.stride=stride; arrivalStride=stride; pdmDetectedStride=0; clockUs=0;
 pdmsw::init(&filter,false);
 for(auto word:raw) for(int copy=0;copy<(stride==2?2:1);++copy){dma.push_back(word&255);dma.push_back(word>>8);}
}
int main(){
 std::vector<uint16_t> raw;
 uint32_t rng=12;
 for(int i=0;i<4096;++i){rng=rng*1664525u+1013904223u;raw.push_back(rng>>16);}
 pdmsw::Decimator ref; pdmsw::init(&ref,false); int32_t expected[1024];
 assert(pdmsw::process(&ref,raw.data(),raw.size(),expected,1024)==1024);
 for(int stride:{1,2}){
  reset(raw,stride); shortReads=true; i2s_32bit_buffer[512]=123456;
  uint64_t us=0;
  assert(readPdmSoftwareBlock(us)==512 && us>0 && i2s_32bit_buffer[512]==123456);
  assert(std::equal(expected,expected+512,i2s_32bit_buffer));
  assert(readPdmSoftwareBlock(us)==512 && std::equal(expected+512,expected+1024,i2s_32bit_buffer));
  assert(readPdmSoftwareBlock(us)==0); // empty DMA returns, no busy loop
 }
 reset(raw,1); shortReads=false; timedOut=true; uint64_t us=0;
 assert(readPdmSoftwareBlock(us)==256); // retain bytes supplied with a timeout
 audioProducerStopRequested=true; size_t before=dmaPos;
 assert(readPdmSoftwareBlock(us)==0 && dmaPos==before);
 // Rate detection works with constant data: it does not depend on audio content.
 for(int stride:{1,2}){
  reset(std::vector<uint16_t>(160000,0xAAAA),stride);
  pdmWordRate.reset(); pdmFramer.stride=2; pdmDetectedOrder=0; shortReads=false;
  int reads=0;
  while(pdmDetectedStride==0 && dmaPos<dma.size()) {readPdmSoftwareBlock(us);assert(++reads<1000);}
  assert(pdmDetectedStride==stride && pdmFramer.stride==stride);
  assert(pdmStrideLogPending==stride);
 }
 // IM73D122 need not return equal slot values. Keep the left, not the right.
 reset(raw,2); shortReads=true;
 for(size_t i=0;i<raw.size();++i){dma[4*i+2]^=0x35;dma[4*i+3]^=0x67;}
 assert(readPdmSoftwareBlock(us)==512 && std::equal(expected,expected+512,i2s_32bit_buffer));
 // Reconfiguration discards all carry state.
 pdmFramer.hasByte=true; pdmFramer.secondOfPair=true; pdmFramer.reset();
 assert(!pdmFramer.hasByte && !pdmFramer.secondOfPair && pdmFramer.stride==2);
}
''')


if __name__ == '__main__':
    unittest.main()
