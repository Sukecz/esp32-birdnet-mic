"""Execute production mode/allocation paths across all four XIAO memory profiles."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()


def function(signature):
    start = SOURCE.index(signature + ' {')
    end = SOURCE.index('{', start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


class ModeMemoryTests(unittest.TestCase):
    def native(self, code, flags=()):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', *flags,
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_actual_capture_budget_and_mode_cycles(self):
        helpers = '\n'.join(function(sig) for sig in (
            'static void deleteAudioRingBuffer()',
            'static RingbufHandle_t createInternalAudioRing(size_t bytes)',
            'bool startAudioProducer()'))
        external = function('static bool createPsramAudioRing(size_t requested)')
        for board, psram in [('C3', False), ('C6', False), ('S3', True),
                             ('C5', True), ('S3', False), ('C5', False)]:
            with self.subTest(board=board, psram=psram):
                self.native(r'''
#include "AudioMemory.h"
#include "AudioReserve.h"
#include "RecordingMemory.h"
#include "PreviewBuffer.h"
#include <cassert>
#include <cstdint>
#include <string>
constexpr uint32_t MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4;
constexpr int RINGBUF_TYPE_BYTEBUF=0,pdPASS=1;
using BaseType_t=int;
struct Ring {size_t bytes;bool external;};
using RingbufHandle_t=Ring*;
Ring* audioRingBuffer=nullptr;
bool audioRingInPsram=false,failExternal=false,failTask=false;
size_t audioRingBufferCapacityBytes=0,internalFree=160*1024,internalLargest=120*1024,externalLargest=8*1024*1024;
int rings=0,currentBufferSize=512;
bool audioProducerStopRequested=false;
uint32_t audioProducerLastReadMs=0;
void* audioProducerTaskHandle=nullptr;
namespace recording {bool active=false;bool mode(){return active;}}
std::string String(uint32_t n){return std::to_string(n);}
void simplePrintln(const std::string&){}
size_t heap_caps_get_free_size(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?externalLargest:internalFree;}
size_t heap_caps_get_largest_free_block(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?externalLargest:internalLargest;}
Ring* xRingbufferCreateWithCaps(size_t n,int,uint32_t caps){
 bool external=caps&MALLOC_CAP_SPIRAM;
 if(external&&failExternal)return nullptr;
 if(!external){if(internalFree<n+1024)return nullptr;internalFree-=n+1024;}
 ++rings;return new Ring{n,external};
}
Ring* xRingbufferCreate(size_t n,int type){return xRingbufferCreateWithCaps(n,type,MALLOC_CAP_INTERNAL);}
void vRingbufferDeleteWithCaps(Ring* r){if(!r->external)internalFree+=r->bytes+1024;--rings;delete r;}
void vRingbufferDelete(Ring* r){assert(!r->external);vRingbufferDeleteWithCaps(r);}
size_t computeAudioRingBufferCapacity(){return 65536;}
void updateHighpassCoeffs(){}
uint32_t millis(){return 1;}
void audioProducerTask(void*){}
int xTaskCreate(void(*)(void*),const char*,size_t stack,void*,int,void** handle){
 if(failTask)return 0;
 assert(internalFree>=stack+1024);internalFree-=stack+1024;*handle=(void*)1;return pdPASS;
}
void stopAudioProducer();
''' + '\n#if AUDIO_RING_IN_PSRAM\n' + external + '\n#endif\n' + helpers + r'''
void stopAudioProducer(){
 if(audioProducerTaskHandle){internalFree+=audioreserve::kProducerStackBytes+1024;audioProducerTaskHandle=nullptr;}
 deleteAudioRingBuffer();audioRingBufferCapacityBytes=0;
}
int main(){
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C5)
 assert(bool(AUDIO_RING_IN_PSRAM)==bool(CONFIG_SPIRAM));
#else
 assert(!AUDIO_RING_IN_PSRAM);
#endif
#if AUDIO_RING_IN_PSRAM
 assert(PreviewBuffer::capacity==4096);
#endif
 // Reserve the independent Listen stack plus conservative task metadata.
 internalFree-=3072+2048;
 const size_t baseline=internalFree;
 for(int n=0;n<50;++n){
  recording::active=false;assert(startAudioProducer());
  assert(audioRingBuffer&&audioRingBufferCapacityBytes==65536);
  assert(audioRingInPsram==bool(AUDIO_RING_IN_PSRAM));
  recording::active=true;assert(startAudioProducer());
  assert(!audioRingBuffer&&!audioRingBufferCapacityBytes&&!rings);
  assert(internalFree==baseline-audioreserve::kProducerStackBytes-1024);
 }
 stopAudioProducer();assert(internalFree==baseline&&!rings);
 // PSRAM absent/fragmented must fall back to a bounded internal Streaming ring.
 recording::active=false;externalLargest=0;assert(startAudioProducer());assert(!audioRingInPsram);
 stopAudioProducer();externalLargest=8*1024*1024;failExternal=true;
 assert(startAudioProducer());assert(!audioRingInPsram);stopAudioProducer();failExternal=false;
 // Failed task creation releases its ring in both placement modes.
 failTask=true;assert(!startAudioProducer());assert(internalFree==baseline&&!rings);
 failTask=false;recording::active=true;internalFree=54*1024;
 assert(!startAudioProducer()&&!rings&&!audioProducerTaskHandle);
 internalFree=57*1024;assert(startAudioProducer());
 assert(internalFree>=recordingmemory::kWorkingReserveBytes);stopAudioProducer();
 internalFree=baseline;internalLargest=1024;assert(!startAudioProducer());
}
''', ['-DCONFIG_IDF_TARGET_ESP32' + board + '=1', '-DCONFIG_SPIRAM=' + str(int(psram))])

    def test_actual_mode_transaction_releases_before_allocation_and_rolls_back(self):
        self.native(r'''
#include <cassert>
#include <cstdint>
bool capture=true,rtspServerEnabled=true,overheatLatched=false,otaInProgress=false;
bool supported=true,recordingAutoStartPending=false,logOk=true,failStart=false,stoppable=true;
int ringBytes=65536,sdBytes=0,checks=0;
uint8_t s3BoardVariant=0;
namespace recording {
 bool active=false,working=false;
 enum class State {Ready,Error};
 struct Status {State state=State::Ready;};
 Status status(){return {};}
 bool mode(){return active;}bool busy(){return working;}
 bool stop(){working=false;return stoppable;}
 void setRuntimeMode(bool rec){active=rec;}
 bool releaseBuffer(){assert(!capture&&!working);sdBytes=0;return true;}
 bool checkCard(bool,uint8_t){assert(capture&&!ringBytes);++checks;return true;}
}
namespace flashlayout {bool blocked=false;bool busy(){return blocked;}}
namespace devicesetup {bool current=false;void currentMode(bool rec){current=rec;}}
bool recordingHardwareSupported(){return supported;}
void cancelThermalResume(){}
bool webui_logModeChanged(bool){return logOk;}
void browserAudioStop(){}
void stopAllRtspClients(const char*){}
struct {void stop(){}void begin(){}void setNoDelay(bool){}} rtspServer;
bool isStreamScheduleAllowedNow(){return true;}
void stopAudioProducer(){capture=false;ringBytes=0;}
bool startAudioProducer(){
 assert(!capture);
 if(failStart){failStart=false;return false;}
 if(!recording::active)assert(!sdBytes);
 ringBytes=recording::active?0:65536;capture=true;return true;
}
void simplePrintln(const char*){}
''' + function('bool setDeviceMode(bool recordingMode)') + r'''
int main(){
 for(int n=0;n<50;++n){
  assert(setDeviceMode(true));assert(capture&&!ringBytes&&!rtspServerEnabled&&devicesetup::current);
  sdBytes=32768;assert(setDeviceMode(false));
  assert(capture&&ringBytes==65536&&!sdBytes&&rtspServerEnabled&&!devicesetup::current);
 }
 assert(checks==50);
 failStart=true;assert(!setDeviceMode(true));
 assert(!recording::active&&capture&&ringBytes==65536&&rtspServerEnabled&&!devicesetup::current&&checks==50);
 logOk=false;assert(!setDeviceMode(true));assert(!recording::active&&capture);logOk=true;
 supported=false;assert(!setDeviceMode(true)&&capture);supported=true;
 stoppable=false;assert(!setDeviceMode(true)&&capture);stoppable=true;
 otaInProgress=true;assert(!setDeviceMode(true));otaInProgress=false;
 flashlayout::blocked=true;assert(!setDeviceMode(true));flashlayout::blocked=false;
 assert(setDeviceMode(true));sdBytes=32768;failStart=true;assert(!setDeviceMode(false));
 assert(recording::active&&capture&&!ringBytes&&!sdBytes&&!rtspServerEnabled&&devicesetup::current);
 overheatLatched=true;stopAudioProducer();assert(setDeviceMode(false));
 assert(!capture&&!rtspServerEnabled); // Mode changes never bypass thermal shutdown.
}
''')


if __name__ == '__main__':
    unittest.main()
