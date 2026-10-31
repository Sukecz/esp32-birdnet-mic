"""Exercise the firmware's allocation, fallback, restart and cleanup paths."""
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


class C5PsramLifecycleTests(unittest.TestCase):
    def test_actual_ring_allocation_task_failure_and_restarts(self):
        code = r'''
#include "AudioReserve.h"
#include "RecordingMemory.h"
#include <cassert>
#include <cstdint>
#include <string>
#define AUDIO_RING_IN_PSRAM 1
#define CONFIG_IDF_TARGET_ESP32C5 1
namespace recording {inline bool recordingMode=false;bool mode(){return recordingMode;}}
constexpr uint32_t MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4;
constexpr int RINGBUF_TYPE_BYTEBUF=0,pdPASS=1;
using BaseType_t=int;
struct Ring {bool external;};
using RingbufHandle_t=Ring*;
Ring* audioRingBuffer=nullptr;
bool audioRingInPsram=false,failExternal=false,failTask=false;
size_t audioRingBufferCapacityBytes=0,internalFree=128*1024,internalLargest=100*1024,externalLargest=8*1024*1024;
int created=0,deleted=0,tasks=0,currentBufferSize=512;
bool audioProducerStopRequested=false;
uint32_t audioProducerLastReadMs=0;
void* audioProducerTaskHandle=nullptr;
std::string String(uint32_t n){return std::to_string(n);}
void simplePrintln(const std::string&){}
size_t heap_caps_get_free_size(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?externalLargest:internalFree;}
size_t heap_caps_get_largest_free_block(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?externalLargest:internalLargest;}
Ring* xRingbufferCreateWithCaps(size_t,int,uint32_t caps){
 assert(caps==((caps&MALLOC_CAP_SPIRAM?MALLOC_CAP_SPIRAM:MALLOC_CAP_INTERNAL)|MALLOC_CAP_8BIT));
 if((caps&MALLOC_CAP_SPIRAM)&&failExternal)return nullptr;
 ++created;return new Ring{bool(caps&MALLOC_CAP_SPIRAM)};
}
Ring* xRingbufferCreate(size_t,int){++created;return new Ring{false};}
void vRingbufferDeleteWithCaps(Ring* r){assert(r);++deleted;delete r;}
void vRingbufferDelete(Ring* r){assert(r&&!r->external);++deleted;delete r;}
size_t computeAudioRingBufferCapacity(){return 65536;}
void updateHighpassCoeffs(){}
uint32_t millis(){return 1;}
void audioProducerTask(void*){}
int xTaskCreate(void(*)(void*),const char*,size_t,void*,int,void**){++tasks;return failTask?0:pdPASS;}
''' + function('static void deleteAudioRingBuffer()') + r'''
void stopAudioProducer(){deleteAudioRingBuffer();audioRingBufferCapacityBytes=0;}
''' + function('static bool createPsramAudioRing(size_t requested)') + '\n' + function('static RingbufHandle_t createInternalAudioRing(size_t bytes)') + '\n' + function('bool startAudioProducer()') + r'''
int main(){
 using namespace audioreserve;
 const size_t minimum=kHeapMargin+kProducerStackBytes+kAllocationOverhead;
 assert(externalHeadroom(minimum,kProducerStackBytes+kAllocationOverhead));
 assert(!externalHeadroom(minimum-1,100*1024));
 assert(!externalHeadroom(128*1024,kProducerStackBytes+kAllocationOverhead-1));
 for(int n=0;n<50;++n){assert(startAudioProducer());assert(audioRingInPsram&&audioRingBufferCapacityBytes==65536);}
 // Restart with a client's TCP buffers still held (Dan's C5 log: 84-86 KB free,
 // largest block 25 KB): the PSRAM ring must still be used under the 64 KB allowance.
 stopAudioProducer();internalFree=84*1024;internalLargest=25*1024;
 assert(startAudioProducer());assert(audioRingInPsram&&audioRingBufferCapacityBytes==65536);
 stopAudioProducer();internalFree=128*1024;internalLargest=100*1024;
 stopAudioProducer();assert(created==deleted&&!audioRingInPsram);
 externalLargest=0;assert(startAudioProducer());assert(!audioRingInPsram&&audioRingBufferCapacityBytes>=16384);
 stopAudioProducer();externalLargest=8*1024*1024;
 failExternal=true;assert(startAudioProducer());assert(!audioRingInPsram);
 stopAudioProducer();failExternal=false;
 failTask=true;assert(!startAudioProducer());assert(!audioRingBuffer&&!audioRingInPsram&&!audioRingBufferCapacityBytes);
 failExternal=true;assert(!startAudioProducer());assert(!audioRingBuffer&&created==deleted);
 failExternal=failTask=false;internalFree=70*1024;
 int before=tasks;assert(!startAudioProducer());assert(tasks==before&&!audioRingBuffer);
 // Recording must start capture and Listen without an RTSP allocation.
 internalFree=100*1024;internalLargest=25*1024;recording::recordingMode=true;
 for(int n=0;n<50;++n){assert(startAudioProducer());assert(!audioRingBuffer&&!audioRingBufferCapacityBytes);}
 stopAudioProducer();assert(created==deleted);
 // A task failure still leaves no buffer; returning to Streaming recreates it.
 failTask=true;assert(!startAudioProducer());failTask=false;
 recording::recordingMode=false;internalFree=128*1024;externalLargest=8*1024*1024;
 assert(startAudioProducer()&&audioRingInPsram);stopAudioProducer();
 before=tasks;internalFree=128*1024;internalLargest=1024;
 assert(!startAudioProducer());assert(tasks==before&&!audioRingBuffer&&created==deleted);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp)/'test.cpp', Path(temp)/'test'
            cpp.write_text(code)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I',str(ROOT/'esp32-birdnet-mic'),
                            str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=20)


if __name__ == '__main__':
    unittest.main()
