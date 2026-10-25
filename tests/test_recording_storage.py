"""Run the real SD worker with fake capture/card faults and real temporary WAV files.

This exercises lifecycle and filesystem behavior on the host. It cannot qualify
SPI timing, card power loss or the physical microphone.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
'Arduino.h': r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
class String : public std::string {
public:
 using std::string::string;
 String(const std::string& s):std::string(s){}
 template<class T,typename=std::enable_if_t<std::is_arithmetic_v<T>>> String(T v):std::string(std::to_string(v)){}
};
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) (void)(p)
#define portEXIT_CRITICAL(p) (void)(p)
#define D6 43
#define D8 7
#define D9 8
#define D10 9
extern uint32_t testNow;
inline uint32_t millis(){return testNow;}
inline void delay(uint32_t n){testNow+=n;}
inline uint32_t esp_random(){static uint32_t id=100;return ++id;}
''',
'Preferences.h': r'''
#pragma once
#include <vector>
#include <cstring>
inline std::vector<uint8_t> savedPrefs;
struct Preferences {
 bool begin(const char*,bool){return true;}
 size_t getBytesLength(const char*){return savedPrefs.size();}
 size_t getBytes(const char*,void* out,size_t n){if(n!=savedPrefs.size())return 0;memcpy(out,savedPrefs.data(),n);return n;}
 size_t putBytes(const char*,const void* p,size_t n){savedPrefs.assign((const uint8_t*)p,(const uint8_t*)p+n);return n;}
 void end(){}
};
''',
'esp_timer.h': '#pragma once\ninline uint64_t esp_timer_get_time(){return uint64_t(testNow)*1000;}\n',
'esp_heap_caps.h': '#pragma once\n#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\ninline size_t heap_caps_get_free_size(int){return 256*1024;}\n',
'freertos/FreeRTOS.h': '#pragma once\n#define pdMS_TO_TICKS(n) (n)\n#define pdPASS 1\n#define pdTRUE 1\n',
'freertos/task.h': r'''
#pragma once
using TaskHandle_t=void*;
struct WorkerDone {};
inline void vTaskDelay(int n){if(!n)throw WorkerDone{};testNow+=n;}
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t*){return pdPASS;}
''',
'freertos/ringbuf.h': r'''
#pragma once
#include <vector>
#include <functional>
#include <algorithm>
struct Ring {std::vector<uint8_t> data;};
using RingbufHandle_t=Ring*;
#define RINGBUF_TYPE_BYTEBUF 1
extern std::function<void()> beforeReceive;
extern bool failQueue;
inline Ring* xRingbufferCreate(size_t,int){return new Ring;}
inline void vRingbufferDelete(Ring* p){delete p;}
inline int xRingbufferSend(Ring* p,const void* b,size_t n,int){if(failQueue)return 0;auto s=(const uint8_t*)b;p->data.insert(p->data.end(),s,s+n);return pdTRUE;}
inline void* xRingbufferReceive(Ring* p,size_t* n,int){*n=p->data.size();if(!*n)return nullptr;auto b=new uint8_t[*n];memcpy(b,p->data.data(),*n);p->data.clear();return b;}
inline void* xRingbufferReceiveUpTo(Ring* p,size_t* n,int,size_t maximum){
 beforeReceive();*n=std::min(p->data.size(),maximum);if(!*n)return nullptr;
 auto b=new uint8_t[*n];memcpy(b,p->data.data(),*n);p->data.erase(p->data.begin(),p->data.begin()+*n);return b;
}
inline void vRingbufferReturnItem(Ring*,void* p){delete[] (uint8_t*)p;}
''',
'SPI.h': '#pragma once\nstruct Spi{void end(){} void begin(int a,int b,int c,int d){if(a!=7||b!=8||c!=9||d!=43)abort();}};inline Spi SPI;\n',
'SD.h': r'''
#pragma once
#include <filesystem>
#include <functional>
#define CARD_NONE 0
extern std::function<void()> mountHook;
extern bool cardMissing;
struct Card {
 std::string root;
 void end(){}
 bool begin(int,Spi&,int,const char* path,int,bool){root=path;mountHook();return !cardMissing;}
 int cardType(){return cardMissing?CARD_NONE:1;}
 uint64_t totalBytes(){return 1ULL<<30;}
 uint64_t usedBytes(){return 0;}
 bool exists(const char* p){return std::filesystem::exists(root+p);}
 bool mkdir(const char* p){return std::filesystem::create_directory(root+p);}
};inline Card SD;
''',
}
HARNESS = r'''
#include <cassert>
#include <functional>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <errno.h>
#include "Arduino.h"
uint32_t testNow=1;
std::function<void()> beforeReceive, mountHook=[]{};
bool failQueue=false,cardMissing=false,failSync=false,shortWrite=false,failRename=false;
int test_fsync(int fd){return failSync?-1:fsync(fd);}
ssize_t test_write(int fd,const void* p,size_t n){if(shortWrite&&n){errno=ENOSPC;return write(fd,p,n-1);}return write(fd,p,n);}
int test_rename(const char* a,const char* b){return failRename?-1:rename(a,b);}
#define fsync test_fsync
#define write test_write
#define rename test_rename
#include "Recording.cpp"
#undef fsync
#undef write
#undef rename
using namespace recording;
void runWorker(){
 // Worker loops forever on-device. Exit its next idle iteration in this harness.
 try{worker(nullptr);}catch(const WorkerDone&){}
}
int receives=0;
void runSession(int fault=0){
 receives=0;
 cardMissing=fault==4;
 mountHook=[fault]{if(fault==5)requestStop();};
 beforeReceive=[fault]{
   ++receives;testNow+=10;
   if(fault==1 && receives==2) failQueue=true;
   if(fault==2 && receives==2) shortWrite=true;
   if(fault==6 && receives==2) captureFault();
   static int16_t samples[2048]={1,-1,32767,-32768};
   push(samples,2048);
   if(receives==5) requestStop();
 };
 assert(start(8000,true));assert(busy());
 runWorker();assert(!busy());
 failQueue=false;shortWrite=false;cardMissing=false;mountHook=[]{};
}
int main(){
 begin();assert(!mode());auto config=settings();config.recordingMode=1;assert(saveSettings(config));
 assert(!start(8000,false));assert(status().state==State::Error);
 runSession();assert(status().state==State::Saved);assert(status().writtenKiB==20);assert(std::filesystem::exists(SD.root+status().file));
 // Saved WAV metadata reflects bytes actually written, not a guessed duration.
 std::ifstream file(SD.root+status().file,std::ios::binary);uint8_t h[44];file.read((char*)h,44);
 assert(h[34]==16&&h[22]==1);assert(std::filesystem::file_size(SD.root+status().file)==44+5*2048*2);
 runSession(1);assert(status().state==State::Error&&status().droppedSamples==2048);assert(std::string(status().file).find(".part")!=std::string::npos);
 runSession(2);assert(status().state==State::Error);assert(std::string(status().error).find("SD full")!=std::string::npos);
 failSync=true;runSession();assert(status().state==State::Error);failSync=false;
 failRename=true;runSession();assert(status().state==State::Error);failRename=false;
 runSession(4);assert(status().state==State::Error&&!status().cardReady);
 runSession(5);assert(status().state==State::Saved&&status().writtenKiB==0); // Stop during mount cancels start.
 runSession(6);assert(status().state==State::Error);assert(std::string(status().error).find("Microphone")!=std::string::npos);
 // Exact finite duration and rotation: two minute-long PCM files, no empty third segment.
 config=settings();config.durationMinutes=2;config.segmentMinutes=1;assert(saveSettings(config));
 beforeReceive=[] {static int16_t samples[2048]={};testNow+=10;push(samples,2048);};
 assert(start(8000,true));runWorker();assert(status().state==State::Saved);
 assert(sessionBytes==uint64_t(8000)*2*120&&segmentIndex==2);
 assert(wav.bytes()==8000*2*60);
 assert(std::filesystem::file_size(SD.root+status().file)==44+8000*2*60);
 // Collision must never replace an existing complete WAV.
 segmentIndex=0;std::string existing=finalName; // Current name is segment 2.
 segmentIndex=1;assert(!openSegment());assert(std::filesystem::file_size(existing)==44+8000*2*60);
}
'''


class RecordingStorageTests(unittest.TestCase):
    def test_real_worker_writes_and_faults(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            card = tmp / 'card'
            card.mkdir()
            for name, body in STUBS.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            source = (ROOT/'esp32-birdnet-mic/Recording.cpp').read_text()
            # Only substitute the test mountpoint; all worker/lifecycle logic is actual source.
            source = source.replace('"/sd', '"'+str(card))
            # Device idle delay is finite; harness exits after a completed session.
            source = source.replace('if (!starting.load()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }',
                                    'if (!starting.load()) { throw WorkerDone{}; }')
            # Device-visible status paths stay relative to /sd (3 bytes) on hardware.
            source = source.replace('partial+3', 'partial+'+str(len(str(card)))).replace('finalName+3', 'finalName+'+str(len(str(card))))
            (tmp/'Recording.cpp').write_text(source)
            (tmp/'test.cpp').write_text(HARNESS)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-DARDUINO_XIAO_ESP32S3',
                            '-I',str(tmp),'-I',str(ROOT/'esp32-birdnet-mic'),str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True,timeout=15)


if __name__ == '__main__':
    unittest.main()
