"""Real filter service with small host adapters: persistence, reserves and bypass."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class NoiseRuntimeTests(unittest.TestCase):
 def test_actual_service(self):
  headers={
   'Arduino.h':r'''
#pragma once
#include <algorithm>
#include <cstring>
#include <string>
#include <cstdint>
#include <cstdio>
#define IRAM_ATTR
extern uint32_t testNow;
inline uint32_t millis(){return testNow;}
struct String:std::string {
 using std::string::string;String(std::string s):std::string(s){}
 template<class T> explicit String(T n):std::string(std::to_string(n)){}
 String(float n,int):std::string(std::to_string(n)){}
};
''',
   'Preferences.h':r'''
#pragma once
#include <cstdint>
extern bool failWrite;
extern uint32_t stored;
struct Preferences {bool begin(const char*,bool){return true;}void end(){}uint32_t getUInt(const char*,uint32_t fallback){return stored?stored:fallback;}size_t putUInt(const char*,uint32_t value){if(failWrite)return 0;stored=value;return 4;}};
''',
   'esp_heap_caps.h':r'''
#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=4,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
extern size_t freeMemory;extern int allocations;
inline void* heap_caps_malloc(size_t n,int){++allocations;return malloc(n);}
inline void heap_caps_free(void* p){--allocations;free(p);}
inline size_t heap_caps_get_free_size(int){return freeMemory;}
inline size_t heap_caps_get_largest_free_block(int){return freeMemory;}
''',
   'esp_timer.h':r'''
#pragma once
extern uint64_t clockValue,stepUs;
inline uint64_t esp_timer_get_time(){clockValue+=stepUs;return clockValue;}
''',
   'freertos/FreeRTOS.h':r'''
#pragma once
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
''',
   'freertos/semphr.h':r'''
#pragma once
using SemaphoreHandle_t=bool*;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new bool(false);}
inline int xSemaphoreTake(SemaphoreHandle_t s,int){if(*s)return 0;*s=true;return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t s){*s=false;}
inline void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
'''}
  code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
uint32_t testNow=0,stored=0;
bool failWrite=false;size_t freeMemory=200000;int allocations=0;
uint64_t clockValue=0,stepUs=100;
#include "NoiseShadow.cpp"
int main(){
 using namespace noiseshadow;
 begin();Chain chain{48000,0,12,true,500,100};int16_t pcm[512]{};
 process(pcm,512,chain);assert(!allocations);assert(statusJson().find("\"mode\":0")!=std::string::npos);
 assert(!save(1,7,15)&&stored==0);failWrite=true;
 assert(!save(1,12,15));assert(statusJson().find("\"mode\":0")!=std::string::npos);
 failWrite=false;assert(save(1,12,15));freeMemory=50000;
 process(pcm,512,chain);assert(!allocations);assert(statusJson().find("Insufficient memory")!=std::string::npos);
 assert(save(0,12,15));process(pcm,512,chain);assert(save(1,12,15));freeMemory=200000;
 process(pcm,512,chain);testNow+=600;process(pcm,512,chain);
 assert(allocations==1&&statusJson().find("\"active\":true")!=std::string::npos);
 // Changing the preceding filter invalidates its learned profile.
 chain.highpassHz=700;process(pcm,512,chain);
 releaseForRestart();assert(allocations==0);process(pcm,512,chain);assert(allocations==1);
 // Sustained overruns fall back to original audio without changing CPU or mode.
 stepUs=20000;
 for(int i=0;i<10;++i){testNow+=600;process(pcm,512,chain);}
 assert(allocations==0);assert(statusJson().find("CPU too slow")!=std::string::npos);
 assert(save(0,12,15));process(pcm,512,chain);assert(allocations==0);
}
'''
  with tempfile.TemporaryDirectory() as t:
   for name,body in headers.items():
    p=Path(t)/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
   cpp=Path(t)/'test.cpp';binary=Path(t)/'test';cpp.write_text(code)
   subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-attributes','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+t,'-I'+str(ROOT/'esp32-birdnet-mic'),str(cpp),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True,timeout=20)
