"""Exercise the actual disabled-RTSP main-loop branch, including Listen gating."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecordingLifecycleTests(unittest.TestCase):
    def test_recording_preserves_listen_with_rtsp_off(self):
        source = (ROOT/'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
        start = source.index('    } else if (!recording::mode()) {', source.index('// RTSP client management'))
        end = source.index('    uint32_t rateNow', start)
        code = r'''
#include <cassert>
#include <cstdint>
namespace recording { bool active=false;bool mode(){return active;} }
bool rtspServerEnabled=false,overheatLatched=false,audioProducerRunning=true,audioProducerStopRequested=false;
bool ap=false,listenAllowed=false;int stops=0;
constexpr int WL_CONNECTED=1;
struct {int state=WL_CONNECTED;int status(){return state;}} WiFi;
uint32_t currentSampleRate=48000;
bool wifiApRecoveryActive(){return ap;}
void stopAllRtspClients(const char*){++stops;}
void refreshLegacyRtspState(){}
void browserAudioLoop(bool allowed,uint32_t){listenAllowed=allowed;}
void service(){if(rtspServerEnabled){
''' + source[start:end] + r'''
}
int main(){
 recording::active=true;
 for(int i=0;i<100;++i){service();assert(stops==0&&listenAllowed);}
 WiFi.state=0;ap=true;service();assert(listenAllowed&&stops==0);
 ap=false;service();assert(!listenAllowed); // No network available.
 ap=true;overheatLatched=true;service();assert(!listenAllowed);
 overheatLatched=false;audioProducerRunning=false;service();assert(!listenAllowed);
 audioProducerRunning=true;audioProducerStopRequested=true;service();assert(!listenAllowed);
 audioProducerStopRequested=false;recording::active=false;service();assert(stops==1&&!listenAllowed);
 rtspServerEnabled=true;service();assert(stops==1&&listenAllowed);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True)


if __name__ == '__main__':
    unittest.main()
