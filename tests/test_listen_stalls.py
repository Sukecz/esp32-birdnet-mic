"""Regress the real preview sender: temporary backpressure versus dead peers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ListenStallTests(unittest.TestCase):
    def test_transient_stall_preserves_connection_and_pcm(self):
        source = r'''
#include "Arduino.h"
#include "WiFi.h"
#include "BrowserAudio.h"
#include <cassert>
#include <cerrno>
#include <string>
#include <signal.h>
#include <sys/syscall.h>
uint32_t testNow=100;
int blocked=-1;bool refuse=false;
extern "C" ssize_t send(int fd,const void* data,size_t bytes,int flags){
 if(refuse&&fd==blocked){errno=EAGAIN;return -1;}
 return syscall(SYS_sendto,fd,data,bytes,flags,nullptr,0);
}
const char* request="GET /listen.wav HTTP/1.1\r\nHost: mic.local:81\r\nOrigin: http://mic.local\r\n\r\n";
int main(){
 signal(SIGPIPE,SIG_IGN);browserAudioBegin();
 int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
 testConnections.emplace_back(pair[0]);assert(send(pair[1],request,strlen(request),0)>0);
 browserAudioLoop(true,48000);char bytes[8192];assert(recv(pair[1],bytes,sizeof(bytes),MSG_DONTWAIT)>44);
 const int16_t first[]={1234,-2345,32767,-32768};
 blocked=pair[0];refuse=true;browserAudioPush(first,4);browserAudioLoop(true,48000);
 // TCP can recover after >500 ms; capture keeps feeding the bounded queue.
 for(int n=0;n<30;++n){testNow+=50;browserAudioPush(first,4);browserAudioLoop(true,48000);assert(browserAudioActive());}
 // Keep waiting beyond the capture-starvation deadline: this is network
 // backpressure with PCM pending, not a missing microphone.
 testNow+=1000;browserAudioLoop(true,48000);assert(browserAudioActive());
 refuse=false;browserAudioLoop(true,48000);
 int got=recv(pair[1],bytes,sizeof(bytes),MSG_DONTWAIT);assert(got>=int(sizeof(first)));
 assert(memcmp(bytes,first,sizeof(first))==0);
 assert(browserAudioDiagnostics().sendTimeouts==0);
 assert(browserAudioDiagnostics().noAudioTimeouts==0);
 assert(browserAudioDiagnostics().maxSendStallMs>=2500);
 assert(browserAudioDiagnostics().sendRecoveries==1);
 // A stalled service must try a writable socket before declaring a timeout.
 browserAudioPush(first,4);testNow+=6000;browserAudioLoop(true,48000);assert(browserAudioActive());
 // A genuinely blocked peer is still released within the fixed deadline.
 refuse=true;browserAudioPush(first,4);browserAudioLoop(true,48000);
 testNow+=5001;browserAudioLoop(true,48000);assert(!browserAudioActive());
 assert(std::string(browserAudioDiagnostics().lastStop)=="send_timeout");
 browserAudioStop();close(pair[1]);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)
            (p/'test.cpp').write_text(source)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                            '-fsanitize=undefined','-fno-sanitize-recover=all',
                            '-I',str(ROOT/'tests/browser_audio_stubs'),'-I',str(ROOT/'esp32-birdnet-mic'),
                            str(p/'test.cpp'),str(ROOT/'esp32-birdnet-mic/BrowserAudio.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=15)


if __name__=='__main__':
    unittest.main()
