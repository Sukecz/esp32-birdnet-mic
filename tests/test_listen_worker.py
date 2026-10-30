"""Run the actual independent Listen worker while the application loop stalls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ListenWorkerTests(unittest.TestCase):
    def test_worker_owns_transport_during_main_loop_stall_and_concurrent_stop(self):
        self.run_transport(False)

    def test_task_creation_failure_keeps_bounded_synchronous_fallback(self):
        self.run_transport(True)

    def run_transport(self, fail):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            arduino = (ROOT / 'tests/browser_audio_stubs/Arduino.h').read_text()
            arduino = arduino.replace('extern uint32_t testNow;\ninline uint32_t millis() { return testNow; }',
                '#include <chrono>\ninline uint32_t millis(){using namespace std::chrono;return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());}')
            (p / 'Arduino.h').write_text(arduino)
            wifi = (ROOT / 'tests/browser_audio_stubs/WiFi.h').read_text().replace(
                'inline std::deque<WiFiClient> testConnections;', r'''
#include <mutex>
struct Connections {
 std::mutex lock;std::deque<WiFiClient> data;
 void emplace_back(int fd){std::lock_guard<std::mutex> guard(lock);data.emplace_back(fd);}
 bool empty(){std::lock_guard<std::mutex> guard(lock);return data.empty();}
 WiFiClient front(){std::lock_guard<std::mutex> guard(lock);return data.front();}
 void pop_front(){std::lock_guard<std::mutex> guard(lock);data.pop_front();}
};inline Connections testConnections;
''')
            (p / 'WiFi.h').write_text(wifi)
            (p / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET_ESP32C5 1\n#define CONFIG_SPIRAM 0\n')
            (p / 'freertos').mkdir()
            (p / 'freertos/FreeRTOS.h').write_text('#pragma once\n#define pdMS_TO_TICKS(n) (n)\n#define pdPASS 1\n')
            (p / 'freertos/task.h').write_text(r'''
#pragma once
#include <thread>
#include <chrono>
#include <atomic>
using TaskHandle_t=void*;
inline bool failListenTask=false;
inline int taskCreates=0;
inline int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,TaskHandle_t* task){
 ++taskCreates;if(failListenTask)return 0;
 *task=(void*)1;std::thread([=]{fn(arg);}).detach();return pdPASS;
}
inline void vTaskDelay(int n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t){return 1234;}
''')
            (p / 'test.cpp').write_text(r'''
#include <Arduino.h>
#include <WiFi.h>
#include "BrowserAudio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <future>
#include <signal.h>
int main(){
 signal(SIGPIPE,SIG_IGN);
 failListenTask=FAIL_TASK;
 browserAudioBegin();
 assert(browserAudioDiagnostics().independentWorker==!bool(FAIL_TASK));
 assert(taskCreates==1);
 if(!FAIL_TASK){browserAudioBegin();assert(taskCreates==1);}
 browserAudioLoop(true,48000);
 int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
 testConnections.emplace_back(sockets[0]);
 const char* request="GET /listen.wav HTTP/1.1\r\nHost: mic.local:81\r\nOrigin: http://mic.local\r\n\r\n";
 assert(send(sockets[1],request,strlen(request),0)==int(strlen(request)));
 if(FAIL_TASK)browserAudioLoop(true,48000);
 for(int n=0;n<1000&&!browserAudioActive();++n)vTaskDelay(1);
 assert(browserAudioActive());
 std::atomic<bool> produce{true};std::atomic<size_t> total{0};
 std::thread reader([&]{
  std::string bytes;size_t pcmAt=0,checked=0;char block[8192];
  for(;;){
   int got=recv(sockets[1],block,sizeof(block),MSG_DONTWAIT);
   if(got==0)break;
   if(got<0){assert(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);vTaskDelay(1);continue;}
   bytes.append(block,got);
   if(!pcmAt){auto at=bytes.find("\r\n\r\n");if(at==std::string::npos||bytes.size()<at+48)continue;pcmAt=at+48;assert(bytes.substr(at+4,4)=="RIFF");}
   while(pcmAt+checked<bytes.size()){assert(uint8_t(bytes[pcmAt+checked])==(checked%2?0x34:0x12));++checked;}
   total.store(checked);
  }
 });
 int16_t pcm[384];for(auto& n:pcm)n=0x3412;
 if(FAIL_TASK){
  for(int n=0;n<100;++n){browserAudioPush(pcm,384);browserAudioLoop(true,48000);vTaskDelay(8);assert(browserAudioActive());}
 }else{
  std::thread producer([&]{while(produce.load()){browserAudioPush(pcm,384);vTaskDelay(8);}});
  // No application-loop service for 900 ms, far longer than the 4 KiB queue.
  vTaskDelay(900);
  assert(browserAudioActive()&&total.load()>40000);
  assert(!browserAudioDiagnostics().overflows);
  // Two simultaneous stop requests must both observe the worker's acknowledgement.
  auto first=std::async(std::launch::async,[]{browserAudioStop();});
  auto second=std::async(std::launch::async,[]{browserAudioStop();});
  assert(first.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  assert(second.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  first.get();second.get();produce.store(false);producer.join();
 }
 browserAudioStop();reader.join();close(sockets[1]);
 assert(!browserAudioActive()&&total.load()>40000);
 std::fflush(stdout);std::_Exit(0); // The real firmware task intentionally lives until reboot.
}
''')
            binary = p / 'test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-DARDUINO_ARCH_ESP32=1', '-DFAIL_TASK=' + str(int(fail)),
                            '-I', str(p), '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(p / 'test.cpp'), str(ROOT / 'esp32-birdnet-mic/BrowserAudio.cpp'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
