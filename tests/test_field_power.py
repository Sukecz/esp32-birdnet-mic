"""Battery policy, persisted configuration, and actual save-before-sleep loop."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_device_setup import STUBS

ROOT = Path(__file__).resolve().parents[1]


def function(signature):
    source = (ROOT/'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
    start = source.index(signature+' {')
    end, depth = source.index('{',start)+1, 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]


class FieldPowerTests(unittest.TestCase):
    def run_cpp(self, code):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            arduino = STUBS['Arduino.h'].replace('struct ESPMock {', 'struct ESPMock {void restart(){} ')
            (tmp/'Arduino.h').write_text(arduino+'\nconstexpr int A0=0;\n')
            (tmp/'Preferences.h').write_text(STUBS['Preferences.h'])
            (tmp/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-fsanitize=undefined','-fno-sanitize-recover=all',
                            '-DARDUINO_XIAO_ESP32C6','-I',str(tmp),'-I',str(ROOT/'esp32-birdnet-mic'),
                            str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True,timeout=10)

    def test_threshold_hysteresis_freshness_latch_and_clock_wrap(self):
        self.run_cpp(r'''
#include "PowerPolicy.h"
#include <cassert>
using namespace fieldpower;
int main(){
 Settings s;assert(valid(s));LowBattery low;
 assert(low.canStart(s,false,0));s.guardEnabled=1;
 assert(!low.canStart(s,false,4000)&&!low.canStart(s,true,3500)&&low.canStart(s,true,3501));
 assert(!low.update(s,true,true,3500,1,100));assert(low.pending);
 assert(!low.update(s,true,true,3500,1,30100)); // same ADC batch cannot confirm a shutdown.
 assert(!low.update(s,true,true,3550,2,30100)); // hysteresis band does not confirm by itself.
 assert(low.update(s,true,true,3499,3,40100));assert(low.latched);
 assert(!low.update(s,true,true,3000,4,50100)); // fires only once.
 assert(!low.canAcknowledge(s,true,3600)&&low.canAcknowledge(s,true,3601));
 assert(!low.canAcknowledge(s,false,4200));low.reset();
 assert(!low.update(s,true,true,3400,5,0xfffffff0));
 assert(!low.update(s,true,true,3700,6,100));assert(!low.pending);
 assert(!low.update(s,true,true,3400,7,0xfffffff0));
 assert(low.update(s,true,true,3400,8,uint32_t(0xfffffff0+30000u)));
 low.reset();s.confirmSeconds=0;
 assert(!low.update(s,false,true,3400,9,100)); // Outside Recording mode, the guard does not shut down.
 assert(!low.update(s,true,false,0,10,100)); // missing ADC is never zero volts.
 assert(low.update(s,true,true,3500,11,101));
 s.guardEnabled=0;assert(!low.update(s,true,true,3000,12,102)&&!low.latched);
 for(int cutoff: {2999,4201,65535}){s.cutoffMv=cutoff;assert(!valid(s));}
}
'''.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>'))

    def test_actual_configuration_and_save_before_sleep(self):
        code = r'''
#include "FieldPower.cpp"
namespace battery {inline Reading sample{true,4000,1};Reading reading(bool,uint8_t){return sample;}}
namespace devicesetup {inline bool offline=false;bool batteryMonitoring(){return true;}bool wifiSuspended(){return offline;}const char* wifiOffError(){return "";}}
namespace recording {
 enum class State {Ready, Starting, Recording, Saving, Saved, Error, Cooling};
 struct Status{State state=State::Recording;uint32_t writtenKiB=0;};
 inline Status view;inline bool working=true,pending=false;
 inline bool recordingMode=true;bool mode(){return recordingMode;}bool busy(){return working;}bool resumePending(){return pending;}
 Status status(){return view;}
 enum class StopReason{LowBattery};
 void requestStop(StopReason=StopReason::LowBattery){if(working)view.state=State::Saving;}
}
bool recordingAutoStartPending=false,overheatLatched=false;
bool fieldPowerStopRequested=false,fieldPowerSaveFinished=false,fieldTestRadioSeen=false;
uint32_t fieldTestAt=0,fieldTestWake=0,fieldTestOfflineWake=0,fieldTestBaseKiB=0,scheduledRebootAt=0;
uint8_t s3BoardVariant=0;
int sleeps=0,pauses=0,timers=0;uint32_t receipt=0;
void cancelThermalResume(){recording::pending=false;}
void simplePrintln(const char*){}
void flushRecordingLogs(){}
void pauseAudioCapture(){assert(!recording::busy());++pauses;}
constexpr int ESP_SLEEP_WAKEUP_ALL=0;
void esp_sleep_disable_wakeup_source(int){timers=0;}
void esp_sleep_enable_timer_wakeup(uint64_t){++timers;}
void esp_deep_sleep_start(){assert(pauses&&!recording::busy());++sleeps;}
void storeFieldPowerReceipt(uint32_t kind){receipt=kind;}
bool startRecording(){recording::working=true;return true;}
'''+function('static void sleepRecordingDevice(uint32_t receipt, const char* reason)')+function('static void serviceFieldPower()')+r'''
int main(){
 using namespace fieldpower;
 begin();assert(!settings().guardEnabled&&nvs["fieldpower/settings"].empty());
 auto s=settings();s.guardEnabled=1;s.confirmSeconds=0;
 assert(saveSettings(s));const auto previous=nvs;failWrite=true;
 assert(saveSettings(s)); // same values succeed without touching NVS.
 s.cutoffMv=3600;assert(!saveSettings(s)&&settings().cutoffMv==3500&&nvs==previous);failWrite=false;
 serviceFieldPower();assert(!triggered()&&!sleeps);
 battery::sample={false,0,2};serviceFieldPower();assert(!triggered()&&!sleeps);
 battery::sample={true,3400,3};serviceFieldPower();assert(triggered()&&!sleeps&&recording::busy());
 serviceFieldPower();assert(!sleeps); // worker still owns the open file.
 recording::working=false;recording::view.state=recording::State::Saved;
 serviceFieldPower();assert(sleeps==1&&pauses==1&&timers==0); // ordinary sleep has no wake timer.
 assert(!canStart(battery::sample));battery::sample={true,3600,4};assert(!acknowledge(battery::sample));
 battery::sample.millivolts=3700;assert(acknowledge(battery::sample)&&canStart(battery::sample));
 s=settings();s.sleepAfterSave=0;assert(saveSettings(s));recording::working=true;recording::view.state=recording::State::Recording;
 battery::sample={true,3300,5};serviceFieldPower();recording::working=false;
 recording::view.state=recording::State::Error;serviceFieldPower();assert(sleeps==1&&pauses==1);
 assert(statusJson(battery::sample).find("save_failed")!=std::string::npos);
 s.cutoffMv=3450;assert(saveSettings(s));recording::working=false;
 recording::view.state=recording::State::Ready;recordingAutoStartPending=true;
 battery::sample={false,0,6};serviceFieldPower();assert(recordingAutoStartPending&&!recording::working);
 battery::sample={true,4000,7};serviceFieldPower();assert(!recordingAutoStartPending&&recording::working);
 // Regression: enabled protection must work after completion/error and before the first recording too.
 recording::working=false;s.sleepAfterSave=1;assert(saveSettings(s));
 for(auto state:{recording::State::Ready,recording::State::Saved,recording::State::Error}){
  battery::sample={true,4000,8};assert(acknowledge(battery::sample));
  recording::view.state=state;battery::sample={true,3300,9};const int before=sleeps;
  serviceFieldPower();assert(triggered()&&sleeps==before+1&&timers==0);
 }
 battery::sample={true,4000,10};assert(acknowledge(battery::sample));
 recording::recordingMode=false;battery::sample={true,3300,11};const int before=sleeps;
 serviceFieldPower();assert(!triggered()&&sleeps==before);recording::recordingMode=true;
 // Offline end sleep is independent of the optional battery guard and ADC availability.
 s.guardEnabled=0;assert(saveSettings(s));battery::sample={false,0,12};devicesetup::offline=true;
 recording::view.state=recording::State::Saved;recording::working=true;
 serviceFieldPower();assert(sleeps==before); // storage worker still owns files/journal.
 recording::working=false;recording::pending=true;serviceFieldPower();assert(sleeps==before);
 recording::pending=false;recording::view.state=recording::State::Cooling;serviceFieldPower();assert(sleeps==before);
 recording::view.state=recording::State::Saved;scheduledRebootAt=1;serviceFieldPower();assert(sleeps==before);scheduledRebootAt=0;
 fieldTestAt=100000;serviceFieldPower();assert(sleeps==before);fieldTestAt=0;
 recordingAutoStartPending=true;overheatLatched=true;serviceFieldPower();assert(sleeps==before);
 recordingAutoStartPending=false;overheatLatched=false;
 serviceFieldPower();assert(sleeps==before+1&&timers==0);
 recording::view.state=recording::State::Error;fieldTestOfflineWake=20;
 serviceFieldPower();assert(sleeps==before+2&&timers==1&&receipt==3);fieldTestOfflineWake=0;
 devicesetup::offline=false;serviceFieldPower();assert(sleeps==before+2);


}
'''
        self.run_cpp(code)

    def test_real_settings_endpoint_validation_and_mutation_guards(self):
        source=(ROOT/'esp32-birdnet-mic/WebUI.cpp').read_text()
        def web_function(signature):
            start=source.index(signature+' {');end=source.index('{',start)+1;depth=1
            while depth:
                depth+=(source[end]=='{')-(source[end]=='}');end+=1
            return source[start:end]
        code=r"""
#include "FieldPower.cpp"
#include <cerrno>
#include <cstdlib>
#define F(x) x
bool auth=true;
struct Web {
 std::map<std::string,String> args;int code=0;String response;
 bool hasArg(const char* k){return args.count(k);}
 String arg(const char* k){return args[k];}
 String uri(){return "/api/field_power";}
 void sendHeader(const char*,const char*){}
 void send(int c,const char*,const String& s){code=c;response=s;}
} web;
bool hasMutationAuth(){return auth;}
namespace recording {inline bool working=false;bool busy(){return working;}}
namespace flashlayout {bool busy(){return false;}}
namespace devicesetup {inline bool monitor=true;bool batteryMonitoring(){return monitor;}}
uint8_t s3BoardVariant=0;
void apiSendJSON(const String& s){web.send(200,"application/json",s);}
"""+web_function('static bool requireMutationAuth(bool preparation = false)')+web_function('static bool parseUInt32Strict(const String& input, uint32_t& out)')+web_function('static void httpFieldPowerSettings()')+r"""
int main(){
 fieldpower::begin();web.args={{"enabled","1"}};const auto initial=nvs;
 auth=false;httpFieldPowerSettings();assert(web.code==403&&nvs==initial);auth=true;
 recording::working=true;httpFieldPowerSettings();assert(web.code==409&&nvs==initial);recording::working=false;
 for(const char* value:{"2999","4201","65536","-1","3500.0","4294970796",""}){
  web.args={{"cutoff_mv",value}};httpFieldPowerSettings();assert(web.code==400&&nvs==initial);
 }
 devicesetup::monitor=false;web.args={{"enabled","1"}};httpFieldPowerSettings();assert(web.code==409&&nvs==initial);devicesetup::monitor=true;
 web.args={{"enabled","1"},{"cutoff_mv","3600"},{"confirm_seconds","17"},{"hysteresis_mv","150"},{"wifi_delay_seconds","0"},{"sleep_after_save","0"}};
 httpFieldPowerSettings();assert(web.code==200&&fieldpower::settings().guardEnabled);
 assert(fieldpower::settings().cutoffMv==3600&&fieldpower::settings().confirmSeconds==17&&!fieldpower::settings().wifiDelaySeconds&&!fieldpower::settings().sleepAfterSave);
 assert(nvs.count("fieldpower/settings"));
}
"""
        self.run_cpp(code)


if __name__=='__main__':
    unittest.main()
