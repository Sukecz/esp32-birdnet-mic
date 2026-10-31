"""Run production SD allocation against the field C5 heap and allocator faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_recording_storage as storage

ROOT = Path(__file__).resolve().parents[1]


class RecordingMemoryTests(unittest.TestCase):
    def test_field_heap_allocation_and_retries(self):
        stubs = dict(storage.STUBS)
        stubs['esp_heap_caps.h'] = r'''
#pragma once
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
inline void* heap_caps_malloc(size_t n,int){return malloc(n);}
inline void heap_caps_free(void* p){free(p);}
inline size_t testInternal=57*1024,testExternal=8*1024*1024;
inline size_t testLargest=33*1024;
inline size_t externalControlCost=0;
inline size_t heap_caps_get_free_size(int caps){return caps&MALLOC_CAP_SPIRAM?testExternal:testInternal;}
inline size_t heap_caps_get_largest_free_block(int caps){return caps&MALLOC_CAP_SPIRAM?testExternal:testLargest;}
'''
        stubs['freertos/task.h'] = r'''
#pragma once
using TaskHandle_t=void*;
struct WorkerDone {};
inline void vTaskDelay(int n){if(!n)throw WorkerDone{};testNow+=n;}
inline bool failTask=false;
inline int xTaskCreate(void(*)(void*),const char*,int stack,void*,int,TaskHandle_t* task){
 if(failTask||testInternal<size_t(stack)+2048||testLargest<size_t(stack)+2048)return 0;
 testInternal-=stack+2048;*task=(void*)1;return pdPASS;
}
'''
        stubs['freertos/ringbuf.h'] = stubs['freertos/ringbuf.h'].replace(
            'struct Ring {std::vector<uint8_t> data;};',
            'struct Ring {std::vector<uint8_t> data;size_t internal=0,external=0;};')
        stubs['freertos/ringbuf.h'] = stubs['freertos/ringbuf.h'].replace(
            'inline Ring* xRingbufferCreate(size_t,int){return new Ring;}',
            '''inline Ring* xRingbufferCreate(size_t n,int){
 if(testInternal<n+1024||testLargest<n+1024)return nullptr;
 auto p=new Ring;p->internal=n+1024;testInternal-=p->internal;return p;
}''').replace('delete p;', 'testInternal+=p->internal;testExternal+=p->external;delete p;')
        stubs['freertos/ringbuf.h'] = stubs['freertos/ringbuf.h'].replace(
            'return failExternalCreate?nullptr:xRingbufferCreate(n,t);',
            '''(void)t;if(failExternalCreate||testExternal<n+1024)return nullptr;
 auto p=new Ring;p->external=n+1024;p->internal=externalControlCost;
 testExternal-=p->external;testInternal-=p->internal;return p;''')
        main = r'''
int main(){
 begin();setRuntimeMode(true);
 // Actual field status with mDNS/AP ON: 57 KiB internal, 33 KiB largest,
 // ample PSRAM. Even a cold SD worker must fit without disabling services.
 assert(start(48000,true));assert(busy()&&queueInPsram);
 assert(status().queueBytes==512*1024&&testInternal>=48*1024);
 running.store(false);starting.store(false);
 const auto initialInternal=testInternal,initialExternal=testExternal;
 for(int n=0;n<100;++n){
  assert(start(48000,true));running.store(false);starting.store(false);
  assert(testInternal==initialInternal&&testExternal==initialExternal);
 }
 // Already-created worker also permits the 54 KiB home/fallback-AP case.
 assert(releaseBuffer());testInternal=54*1024;
 assert(start(48000,true)&&queueInPsram);running.store(false);starting.store(false);
 assert(releaseBuffer());
 // Enforce the reserve using actual post-allocation heap, not just estimates.
 externalControlCost=8*1024;testInternal=54*1024;
 const auto externalBefore=testExternal;
 assert(!start(48000,true)&&!queue&&!busy());
 assert(testInternal==54*1024&&testExternal==externalBefore);externalControlCost=0;
 // A genuinely exhausted internal heap is rejected even with free PSRAM.
 testInternal=47*1024;assert(!start(48000,true)&&!queue&&!busy());
 testInternal=57*1024;
 // Fragmented/failed external allocations never consume the internal reserve.
 failExternalCreate=true;assert(!start(48000,true)&&!queue&&!busy());
 assert(testInternal==57*1024);failExternalCreate=false;
 // Worker failure releases the newly allocated queue, allowing a later retry.
 storageTask=nullptr;failTask=true;
 const auto beforeExternal=testExternal;
 assert(!start(48000,true)&&!queue&&!busy());assert(testExternal==beforeExternal);
 failTask=false;testLargest=7*1024;
 assert(!start(48000,true)&&!queue&&!storageTask);
 testLargest=33*1024;
 assert(start(48000,true)&&queueInPsram);running.store(false);starting.store(false);
 assert(releaseBuffer());
 // No PSRAM: preserve the larger internal reserve and bounded queue.
 testExternal=0;testInternal=79*1024;
 assert(!start(48000,true)&&!queue);
 testInternal=150*1024;testLargest=100*1024;
 assert(start(48000,true)&&!queueInPsram&&testInternal>=72*1024);
 running.store(false);starting.store(false);assert(releaseBuffer());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name, body in stubs.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            (tmp/'test.cpp').write_text(storage.HARNESS.split('int main(){', 1)[0] + main)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DARDUINO_XIAO_ESP32C5', '-DTEST_SENSE=0',
                            '-I', str(tmp), '-I', str(ROOT/'esp32-birdnet-mic'),
                            str(tmp/'test.cpp'), '-o', str(tmp/'test')], check=True)
            subprocess.run([str(tmp/'test')], check=True, timeout=15)


    def test_actual_start_guards_and_api_errors(self):
        def function(source, signature):
            start = source.index(signature + ' {')
            end = source.index('{', start) + 1
            depth = 1
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            return source[start:end]

        sketch = (ROOT/'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
        webui = (ROOT/'esp32-birdnet-mic/WebUI.cpp').read_text()
        guards = r"""
namespace battery { struct Reading {}; Reading reading(bool,uint8_t){return {};}}
namespace devicesetup {bool batteryMonitoring(){return true;}}
namespace fieldpower {
 bool allowed=true;bool canStart(const battery::Reading&){return allowed;}
 bool triggered(){return false;}bool acknowledge(const battery::Reading&){return false;}
}
namespace flashlayout {bool active=false;bool busy(){return active;}}
bool otaInProgress=false,overheatLatched=false,audioProducerRunning=true,audioProducerStopRequested=false;
uint32_t scheduledRebootAt=0,recordingDmaBaseline=0,currentSampleRate=48000;
uint8_t s3BoardVariant=0;
uint32_t getI2sDmaOverrunTotal(){return 42;}
bool recordingHardwareSupported(){return true;}
"""
        api = r"""
#define F(s) String(s)
String reply;
String jsonEscape(const String& value){return value;}
void apiSendJSON(const String& value){reply=value;}
bool requireMutationAuth(){return true;}
struct {
 String action="start";
 String arg(const char* key){return std::string(key)=="action"?action:String();}
 bool hasArg(const char*){return false;}
 void send(int,const char*,const char*){assert(false);}
} web;
bool parseUInt32Strict(const String&,uint32_t&){return false;}
void cancelThermalResume(){}void cancelRecordingAutoStart(){}
bool requestRecordingWifiOff(uint32_t){return false;}
bool requestOfflineSleepTest(uint32_t){return false;}
bool requestBatterySleepTest(uint32_t){return false;}
"""
        main = r"""
int main(){
 begin();setRuntimeMode(true);
 testInternal=47*1024;httpRecordingAction();
 assert(reply.find("Not enough internal RAM for SD")!=std::string::npos);
 assert(reply.find("Recording action failed")==std::string::npos);
 auto blocked=[&](const char* expected){httpRecordingAction();assert(reply.find(expected)!=std::string::npos);assert(std::string(status().error).find(expected)!=std::string::npos);};
 fieldpower::allowed=false;blocked("Battery protection");fieldpower::allowed=true;
 otaInProgress=true;blocked("update is in progress");otaInProgress=false;
 flashlayout::active=true;blocked("update is in progress");flashlayout::active=false;
 overheatLatched=true;blocked("Thermal protection");overheatLatched=false;
 audioProducerRunning=false;blocked("Microphone capture is stopped");audioProducerRunning=true;
 audioProducerStopRequested=true;blocked("Microphone capture is stopped");audioProducerStopRequested=false;
 scheduledRebootAt=1;blocked("restart is pending");scheduledRebootAt=0;
 // Unrelated actions must not inherit a preceding recording error.
 web.action="wifi_off";httpRecordingAction();assert(reply.find("restart is pending")==std::string::npos);
 web.action="start";testInternal=86*1024;httpRecordingAction();
 assert(reply=="{\"ok\":true}"&&busy()&&recordingDmaBaseline==42);
 const auto active=status();httpRecordingAction();assert(reply.find("Recording is busy")!=std::string::npos);
 assert(status().state==active.state&&!status().error[0]);
 assert(!rejectStart("must not replace active recording")&&!status().error[0]);
 web.action="stop";httpRecordingAction();
 assert(reply.find("SD stop timed out; wait before retrying")!=std::string::npos);
 running.store(false);starting.store(false);assert(releaseBuffer());
}
"""
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name, body in storage.STUBS.items():
                path = tmp/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body)
            code = (storage.HARNESS.split('int main(){', 1)[0] + guards +
                    function(sketch, 'bool startRecording()') + api +
                    function(webui, 'static void httpRecordingAction()') + main)
            (tmp/'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DARDUINO_XIAO_ESP32C5', '-DTEST_SENSE=0',
                            '-I', str(tmp), '-I', str(ROOT/'esp32-birdnet-mic'),
                            str(tmp/'test.cpp'), '-o', str(tmp/'test')], check=True)
            subprocess.run([str(tmp/'test')], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
