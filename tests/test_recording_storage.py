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
#if defined(ARDUINO_XIAO_ESP32C3)
#define D6 21
#define D8 8
#define D9 9
#define D10 10
#elif defined(ARDUINO_XIAO_ESP32C5)
#define D6 11
#define D8 8
#define D9 9
#define D10 10
#elif defined(ARDUINO_XIAO_ESP32C6)
#define D6 16
#define D8 19
#define D9 20
#define D10 18
#else
#define D6 43
#define D8 7
#define D9 8
#define D10 9
#endif
extern uint32_t testNow;
inline uint32_t millis(){return testNow;}
extern void (*delayHook)();
inline void delay(uint32_t n){testNow+=n;if(delayHook)delayHook();}
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
'esp_heap_caps.h': '#pragma once\n#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\n#define MALLOC_CAP_SPIRAM 4\ninline void* heap_caps_malloc(size_t n,int){return malloc(n);}\ninline void heap_caps_free(void* p){free(p);}\ninline size_t testInternal=86*1024,testExternal=8*1024*1024;\ninline size_t heap_caps_get_free_size(int caps){return caps & MALLOC_CAP_SPIRAM ? testExternal : testInternal;}\ninline size_t testLargest=1024*1024;\ninline size_t heap_caps_get_largest_free_block(int){return testLargest;}\n',
'freertos/FreeRTOS.h': '#pragma once\n#define pdMS_TO_TICKS(n) (n)\n#define pdPASS 1\n#define pdTRUE 1\n',
'freertos/task.h': r'''
#pragma once
using TaskHandle_t=void*;
struct WorkerDone {};
inline void vTaskDelay(int n){if(!n)throw WorkerDone{};testNow+=n;}
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* task){*task=(void*)1;return pdPASS;}
''',
'freertos/ringbuf.h': r'''
#pragma once
#include <esp_heap_caps.h>
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
inline bool failExternalCreate=false;
inline Ring* xRingbufferCreateWithCaps(size_t n,int t,int caps){if(!(caps&MALLOC_CAP_SPIRAM))abort();return failExternalCreate?nullptr:xRingbufferCreate(n,t);}
inline void vRingbufferDeleteWithCaps(Ring* p){delete p;}
inline int xRingbufferSend(Ring* p,const void* b,size_t n,int){if(failQueue)return 0;auto s=(const uint8_t*)b;p->data.insert(p->data.end(),s,s+n);return pdTRUE;}
inline void* xRingbufferReceive(Ring* p,size_t* n,int){*n=p->data.size();if(!*n)return nullptr;auto b=new uint8_t[*n];memcpy(b,p->data.data(),*n);p->data.clear();return b;}
inline void* xRingbufferReceiveUpTo(Ring* p,size_t* n,int,size_t maximum){
 beforeReceive();*n=std::min(p->data.size(),maximum);if(!*n)return nullptr;
 auto b=new uint8_t[*n];memcpy(b,p->data.data(),*n);p->data.erase(p->data.begin(),p->data.begin()+*n);return b;
}
inline void vRingbufferReturnItem(Ring*,void* p){delete[] (uint8_t*)p;}
''',
'SPI.h': '#pragma once\nextern int expectedCs,spiBegins;\nstruct Spi{void end(){} void begin(int a,int b,int c,int d){if(a!=D8||b!=D9||c!=D10||d!=expectedCs)abort();++spiBegins;}};inline Spi SPI;\n',
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
 bool begin(int cs,Spi&,int hz,const char* path,int files,bool format){if(cs!=expectedCs||(hz!=20000000&&hz!=16000000&&hz!=8000000&&hz!=4000000&&hz!=2000000&&hz!=1000000)||files!=5||format)abort();root=path;mountHook();return !cardMissing;}
 int cardType(){return cardMissing?CARD_NONE:1;}
 uint64_t cardSize(){return 1ULL<<30;}
 bool readRAW(uint8_t* b,uint32_t sector){
  if(sector)return false;
  memset(b,0,512);
  b[510]=0x55;b[511]=0xaa;b[12]=2;b[13]=8;b[14]=32;b[16]=2;
  b[34]=32;b[37]=8;return true;
 }
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
#include <time.h>
void (*delayHook)()=nullptr;
time_t testEpoch=1793018096;
time_t test_time(time_t*){return testEpoch;}
uint32_t testNow=1;
#if defined(ARDUINO_XIAO_ESP32S3) && TEST_SENSE
int expectedCs=21;
#else
int expectedCs=D6;
#endif
int spiBegins=0;
constexpr uint8_t boardVariant=TEST_SENSE;
std::function<void()> beforeReceive, mountHook=[]{};
bool failQueue=false,cardMissing=false,failSync=false,shortWrite=false,failRename=false,corruptRead=false;
size_t maxPosixWrite=0;
int test_fsync(int fd){return failSync?-1:fsync(fd);}
ssize_t test_write(int fd,const void* p,size_t n){if(n>maxPosixWrite)maxPosixWrite=n;if(shortWrite&&n){errno=ENOSPC;return write(fd,p,n-1);}return write(fd,p,n);}
ssize_t test_read(int fd,void* p,size_t n){const ssize_t got=read(fd,p,n);if(corruptRead&&got>0)((uint8_t*)p)[0]^=1;return got;}
int test_rename(const char* a,const char* b){return failRename?-1:rename(a,b);}
#define time test_time
#define fsync test_fsync
#define write test_write
#define read test_read
#define rename test_rename
#include "Recording.cpp"
#undef time
#undef fsync
#undef write
#undef read
#undef rename
using namespace recording;
void runWorker(){
 // Worker loops forever on-device. Exit its next idle iteration in this harness.
 try{worker(nullptr);}catch(const WorkerDone&){}
}
std::vector<std::string> lastLogs;
std::vector<std::string> drainLogs(){
 std::vector<std::string> messages;String line;
 while(nextLog(line))messages.emplace_back(line);
 return messages;
}
bool logged(const char* text){for(const auto& line:lastLogs)if(line.find(text)!=std::string::npos)return true;return false;}
int receives=0;
void runSession(int fault=0){
 receives=0;drainLogs();
 cardMissing=fault==4;
 mountHook=[fault]{if(fault==5)requestStop();};
 beforeReceive=[fault]{
   ++receives;testNow+=10;
   if(fault==7&&receives==2)testNow+=300000;
   if(fault==1 && receives==2) failQueue=true;
   if(fault==2 && receives==2) shortWrite=true;
   if(fault==6 && receives==2) captureFault();
   static int16_t samples[2048]={1,-1,32767,-32768};
   push(samples,2048);
   if(receives==5) requestStop();
 };
 assert(start(8000,true,boardVariant));assert(busy());
 runWorker();assert(!busy());
 lastLogs=drainLogs();
 assert(logSettings().detailed?logged("Recording: start requested"):!logged("Recording: start requested"));
 assert(logSettings().detailed?logged("Recording: finished"):(logged("Recording: stopped")||logged("Recording: failed")));
 if(status().state==State::Error){
  assert(logged("Recording: ERROR")&&logged(status().error)&&logged("partial file preserved"));
  size_t errors=0;for(const auto& line:lastLogs)if(line.find("Recording: ERROR")!=std::string::npos)++errors;
  assert(errors==1); // Cleanup failures must not duplicate/mask the original cause.
 }else{
  assert(logSettings().detailed?(logged("segment saved and verified")&&logged("dropped=0")):logged("Recording: saved"));
  if(fault==5)assert(!logged("Recording: started"));
  else assert(logged("Recording: started"));
 }
 failQueue=false;shortWrite=false;cardMissing=false;mountHook=[]{};
}
int main(){
 setenv("TZ","Europe/Prague",1);tzset();
 assert(recordingpins::resolve(s3boards::External).cs==D6);
#if defined(ARDUINO_XIAO_ESP32S3)
 assert(!recordingpins::resolve(s3boards::AtomS3U).supported);
 assert(!recordingpins::resolve(255).supported);
 assert(recordingpins::resolve(s3boards::Sense).cs==21);
 assert(!checkCard(true,s3boards::AtomS3U)&&spiBegins==0);
#else
 assert(recordingpins::resolve(s3boards::Sense).cs==D6);
#endif
 begin();assert(!mode());assert(!logSettings().detailed&&logSettings().sdEnabled);journalConfig.detailed=1;auto config=settings();config.recordingMode=1;assert(saveSettings(config));
#if defined(ARDUINO_XIAO_ESP32S3)
 assert(!start(8000,true,s3boards::AtomS3U)&&spiBegins==0);
 assert(!checkCard(true,s3boards::AtomS3U)&&spiBegins==0);
#endif
 assert(!start(8000,false));assert(status().state==State::Error);
 // Preflight mounts/unmounts without creating a directory or recording file.
 assert(!checkCard(false));assert(status().state==State::Error);
 // C5-like heap: SD check needs only a worker, no audio queue or PSRAM.
 testExternal=0;testInternal=55*1024;
 assert(!checkCard(true,boardVariant)&&!queue&&!storageTask);
 testInternal=79*1024;
 testLargest=7*1024;assert(!checkCard(true,boardVariant)&&!storageTask);
 testLargest=1024*1024;
 cardMissing=true;assert(checkCard(true,boardVariant));assert(status().state==State::Checking&&busy());
 assert(!start(8000,true,boardVariant));assert(!checkCard(true,boardVariant));runWorker();
 assert(status().state==State::Error&&!status().cardReady&&!busy());
 // A new allocation failure must replace the preceding missing-card error.
 assert(!start(8000,true,boardVariant));
 assert(std::string(status().error)=="Not enough RAM for SD recording buffer");
 cardMissing=false;assert(checkCard(true,boardVariant));runWorker();
 assert(status().state==State::Ready&&status().cardReady&&!busy());
 assert(std::filesystem::exists(SD.root+"/recording.log"));assert(!std::filesystem::exists(SD.root+"/recordings"));assert(!queue);
 // Recording with the same internal heap uses PSRAM; without it allocation
 // fails safely and leaves room for Wi-Fi rather than consuming the margin.
 assert(!start(8000,true,boardVariant)&&!queue);
 testExternal=8*1024*1024;
 // Busy UI leaves 70 KiB internal RAM: PSRAM recording still has room, but
 // a fragmented PSRAM allocator must not spill into the internal reserve.
 testInternal=70*1024;failExternalCreate=true;
 assert(!start(8000,true,boardVariant)&&!queue);
 failExternalCreate=false;
 assert(std::string(statusJson(true,boardVariant)).find(TEST_SENSE?"sense_builtin":"external_spi")!=std::string::npos);

 runSession();assert(queueInPsram);
assert(status().queueBytes==512*1024);
 assert(status().verified&&status().verifiedBytes==44+5*2048*2);testInternal=86*1024;assert(status().state==State::Saved);assert(status().writtenKiB==20);assert(std::filesystem::exists(SD.root+status().file));
 assert(std::string(status().file).find("2026-10-26_13-34-56_")!=std::string::npos);
 assert(std::string(statusJson(true,boardVariant,48000)).find("\"space_seconds\":11184")!=std::string::npos);
 // Saved WAV metadata reflects bytes actually written, not a guessed duration.
 std::ifstream file(SD.root+status().file,std::ios::binary);uint8_t h[44];file.read((char*)h,44);
 assert(h[34]==16&&h[22]==1);assert(std::filesystem::file_size(SD.root+status().file)==44+5*2048*2);
 // Standalone read-back covers >64 KiB and the WAV's unaligned prefix.
assert(kDefaultStorageClockHz==16000000&&kDefaultWriteBlockBytes==4096);
 assert(!setStorageClock(0)&&!setStorageClock(25000000));
 assert(!setStorageClock(2000000,513));
 assert(setStorageClock(2000000,512));maxPosixWrite=0;
 assert(diagnose(true,boardVariant)&&busy());
 assert(!setStorageClock(4000000)&&!diagnose(true,boardVariant));
 runWorker();assert(status().state==State::Ready&&status().diagnostic&&status().verified);
 assert(status().verifiedBytes==1048576+44&&status().spiHz==2000000);
 assert(status().writeBlockBytes==512&&maxPosixWrite==512);
 assert(status().fatBits==32&&status().clusterBytes==4096&&status().cardMiB==1024);
 assert(std::filesystem::file_size(SD.root+status().file)==1048576+44);
 runSession();assert(status().state==State::Saved&&maxPosixWrite==512);
 // Diagnostic fault retains first I/O details and the partial test file.
 assert(diagnose(true,boardVariant));shortWrite=true;runWorker();shortWrite=false;
 assert(status().state==State::Error&&!status().verified&&status().ioErrno==ENOSPC);
 assert(status().ioOffset==0&&status().ioRequested==44&&status().ioReturned==43);
 assert(std::string(status().ioOperation)=="write");
 assert(std::filesystem::file_size(SD.root+status().file)==43);
 assert(diagnose(true,boardVariant));corruptRead=true;runWorker();corruptRead=false;
 assert(status().state==State::Error&&!status().verified&&std::string(status().ioOperation)=="compare");
 assert(setStorageClock(4000000));
 corruptRead=true;runSession();corruptRead=false;
 assert(status().state==State::Error&&!status().verified&&std::string(status().ioOperation)=="wav-header");
 assert(std::string(status().file).find(".part")!=std::string::npos);
 runSession(1);assert(status().state==State::Error&&status().droppedSamples==2048);assert(std::string(status().file).find(".part")!=std::string::npos);
 runSession(2);assert(status().state==State::Error);assert(std::string(status().error).find("SD full")!=std::string::npos);
 failSync=true;runSession();assert(status().state==State::Error);failSync=false;
 failRename=true;runSession();assert(status().state==State::Error);failRename=false;
 runSession(4);assert(status().state==State::Error&&!status().cardReady);
 runSession(5);assert(status().state==State::Saved&&status().writtenKiB==0); // Stop during mount cancels start.
 runSession(7);assert(status().state==State::Saved&&logged("Recording: progress"));
 runSession(6);assert(status().state==State::Error);assert(std::string(status().error).find("Microphone")!=std::string::npos);
 // Exact finite duration and rotation: two minute-long PCM files, no empty third segment.
 config=settings();config.durationMinutes=2;config.segmentMinutes=1;assert(saveSettings(config));
 beforeReceive=[] {static int16_t samples[2048]={};testNow+=10;push(samples,2048);};
 drainLogs();assert(start(8000,true,boardVariant));runWorker();assert(status().state==State::Saved);
 lastLogs=drainLogs();assert(logged("reason=duration limit")&&logged("written=1920000B"));
 size_t savedSegments=0;for(const auto& line:lastLogs)if(line.find("segment saved and verified")!=std::string::npos){++savedSegments;assert(line.find("segment_size=960044B")!=std::string::npos);}
 assert(savedSegments==2);
 assert(sessionBytes==uint64_t(8000)*2*120&&segmentIndex==2);
 assert(wav.bytes()==8000*2*60);
 assert(std::filesystem::file_size(SD.root+status().file)==44+8000*2*60);
 // Collision must never replace an existing complete WAV.
 segmentIndex=0;std::string existing=finalName; // Current name is segment 2.
 segmentIndex=1;assert(!openSegment());assert(std::filesystem::file_size(existing)==44+8000*2*60);
 // Thermal pause drains accepted PCM, saves the first file, resumes in a new
 // file, and counts only audio time toward the original finite duration.
 config=settings();config.durationMinutes=1;assert(saveSettings(config));
 auto pauseSession=[] {
   beforeReceive=[]{};
   mountHook=[] {static int16_t data[1600]={};assert(xRingbufferSend(queue,data,sizeof(data),0));};
   assert(start(8000,true,boardVariant));
   delayHook=[] {delayHook=nullptr;runWorker();};
   assert(pauseForCooling()&&resumePending()&&!busy()&&status().state==State::Cooling);
   mountHook=[]{};
 };
 pauseSession();const std::string beforeFile=status().file;
 assert(std::filesystem::exists(SD.root+beforeFile));assert(!saveSettings(config));
 assert(!checkCard(true,boardVariant));
 testNow+=300000; // The thermal wait must not consume the recording budget.
 beforeReceive=[] {static int16_t data[2048]={};push(data,2048);};
 assert(resume(8000,true,boardVariant));runWorker();
 assert(status().state==State::Saved&&!resumePending()&&status().elapsedSeconds==60);
 assert(completedBytes+sessionBytes==960000&&status().writtenKiB==960000/1024);
 assert(beforeFile!=status().file);
 pauseSession();beforeReceive=[] {static int16_t data[2048]={};push(data,2048);};
 assert(resume(16000,true,boardVariant));runWorker();
 assert(status().elapsedSeconds==60&&sessionBytes==uint64_t(480000-1600)*4);
 pauseSession();cancelResume();assert(!resumePending()&&!resume(8000,true,boardVariant));
 pauseSession();cardMissing=true;assert(resume(8000,true,boardVariant));runWorker();
 assert(status().state==State::Error&&!resumePending());cardMissing=false;
 // Stop must preserve an error even while worker cleanup remains busy.
 running.store(true);error("test SD failure");requestStop();
 assert(status().state==State::Error);running.store(false);
 // Repeated stops during cleanup must not overwrite Saved or flood logs.
 drainLogs();view.state=State::Saved;running.store(true);stopRequested.store(false);
 requestStop();requestStop();assert(status().state==State::Saved&&drainLogs().empty());running.store(false);
 // The event queue is bounded and explicitly reports lost older events.
 for(unsigned i=0;i<kLogCapacity+3;++i)logEvent(LogKind::Opened);
 lastLogs=drainLogs();assert(lastLogs.size()==kLogCapacity+1&&logged("3 older log events dropped"));
 // A delayed successful stop clears an earlier timeout message.
 beforeReceive=[]{};assert(start(8000,true,boardVariant));
 assert(!stop(1));runWorker();assert(status().state==State::Saved&&!status().error[0]);
 // An unknown clock produces an explicit fallback rather than a bogus date.
 testEpoch=0;runSession();assert(std::string(status().file).find("unsynced_")!=std::string::npos);
 // The same worker must also record with small PSRAM or no PSRAM (C3/C6).
 assert(releaseBuffer()&&!queue&&!status().queueBytes);
 testExternal=192*1024;runSession();
 assert(status().state==State::Saved&&queueInPsram&&status().queueBytes==128*1024);
 assert(releaseBuffer()&&!queue&&!status().queueBytes);
 testExternal=0;testInternal=150*1024;runSession();
 assert(status().state==State::Saved&&!queueInPsram&&status().queueBytes>=32768&&status().queueBytes<=128*1024);
 // Returning to Streaming frees the internal queue, then Recording can recreate it.
 assert(releaseBuffer()&&!queue&&!queueInPsram&&!status().queueBytes);
 testInternal=106*1024;runSession();assert(status().state==State::Saved&&status().queueBytes>=32768);
 assert(releaseBuffer());testInternal=150*1024;
 // Normal defaults are concise; the SD file appends across sessions and reads stay on the worker.
 auto logsConfig=logSettings();logsConfig.detailed=0;logsConfig.maxKiB=64;
 assert(saveLogSettings(logsConfig));
 const auto journalFile=SD.root+"/recording.log";
 auto contents=[&] {std::ifstream input(journalFile);return std::string(std::istreambuf_iterator<char>(input),{});};
 const auto oldLog=contents();
 queueSystemLog("[test time] Wi-Fi disabled for battery recording");
 runSession();assert(!logged("Recording: progress")&&!logged("event_uptime_ms"));
 assert(contents().substr(0,oldLog.size())==oldLog);
 const auto appendOne=contents().substr(oldLog.size());
 assert(appendOne.find("Recording: started")!=std::string::npos&&appendOne.find("Recording: stopped")!=std::string::npos);
 assert(appendOne.find("Wi-Fi disabled")!=std::string::npos&&appendOne.find("start requested")==std::string::npos);
 const size_t firstSize=contents().size();runSession();assert(contents().size()>firstSize);
 const auto beforeRead=status();assert(requestLogTail(false,true,boardVariant));runWorker();
 assert(logTailReady()&&logTailError().empty()&&logTail().find("Recording: stopped")!=std::string::npos);
 assert(logTailFileBytes()==contents().size()&&status().state==beforeRead.state&&status().writtenKiB==beforeRead.writtenKiB&&status().verified);
 // Failure of the log file alone must never turn a successful WAV into an SD recording error.
 std::filesystem::rename(journalFile,journalFile+".preserved");std::filesystem::create_directory(journalFile);
 runSession();assert(status().state==State::Saved&&status().verified&&status().logError[0]&&!status().error[0]);
 std::filesystem::remove(journalFile);std::filesystem::rename(journalFile+".preserved",journalFile);
 runSession();assert(!status().logError[0]);
 logsConfig.sdEnabled=0;assert(saveLogSettings(logsConfig));const auto disabledBefore=contents();
 runSession();assert(contents()==disabledBefore&&status().state==State::Saved);



}
'''


class RecordingStorageTests(unittest.TestCase):
    def test_real_worker_writes_and_faults(self):
        for board, sense in [('XIAO_ESP32C3', 0), ('XIAO_ESP32S3', 0), ('XIAO_ESP32S3', 1), ('XIAO_ESP32C5', 0), ('XIAO_ESP32C6', 0)]:
            with self.subTest(board=board, sense=sense):
                self.run_worker(board, sense)

    def run_worker(self, board, sense):
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
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-DARDUINO_'+board,'-DTEST_SENSE='+str(sense),
                            '-I',str(tmp),'-I',str(ROOT/'esp32-birdnet-mic'),str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True,timeout=15)


if __name__ == '__main__':
    unittest.main()
