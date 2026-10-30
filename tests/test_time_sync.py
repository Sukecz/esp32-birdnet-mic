"""Compile the production SNTP callback/configuration/scheduler with a fake network.

No live NTP or device is needed. The fake clock never becomes synchronized merely
because it retains an epoch; tests deliver the actual registered SNTP callback.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_device_setup import STUBS

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino'


def function(source, name):
    match = re.search(r'^(?:static )?(?:bool|void|const char\*)\s+' + name + r'\([^\n]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Missing production function: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREFIX = r'''
#include "Arduino.h"
#include "TimeZoneSchedule.h"
#include <atomic>
#include <sys/time.h>
#include <cstdlib>
constexpr int WL_CONNECTED=3;
namespace devicesetup {inline bool suspended=false;bool wifiSuspended(){return suspended;}}
struct {int state=0;int status(){return state;}} WiFi;
bool validClock=false,backendEnabled=false,backendInitialized=false,restartOk=true,isStreaming=false;
int configCalls=0,restarts=0,stops=0,logs=0;
uint32_t backendInterval=0;
void(*callback)(timeval*)=nullptr;
void simplePrintln(const String&){++logs;}
bool hasValidTime(){return validClock;}
bool getLocalTime(tm*,unsigned){assert(false&&"NTP must never wait on getLocalTime");return false;}
void delay(unsigned){assert(false&&"NTP must never delay audio");}
void esp_sntp_set_time_sync_notification_cb(void(*fn)(timeval*)){callback=fn;}
void esp_sntp_set_sync_interval(uint32_t interval){backendInterval=interval;}
bool esp_sntp_enabled(){assert(backendInitialized);return backendEnabled;}
void esp_sntp_stop(){++stops;backendEnabled=false;}
bool esp_sntp_restart(){assert(backendEnabled);++restarts;return restartOk;}
void configTime(int,int,const char* first,const char* second){assert(first&&second);++configCalls;backendInitialized=backendEnabled=true;}
'''
HARNESS = r'''
void resetState(){
 timeSyncEnabled=true;timeSynced=false;lastTimeSyncAttempt=lastTimeSyncSuccess=0;
 ntpSyncEvents.store(0);ntpConfigured=ntpServiceEnabled=ntpAttempted=ntpWasConnected=ntpReconnectPending=false;
 WiFi.state=0;validClock=backendEnabled=backendInitialized=false;restartOk=true;configCalls=restarts=stops=logs=0;
 clockMs=100;callback=nullptr;timeZoneMode=TIME_ZONE_CENTRAL_EUROPE;
}
void reply(){assert(callback);validClock=true;timeval value{1793178000,0};callback(&value);}
void check(bool condition){assert(condition);}
int main(int argc,char** argv){
 assert(argc==2);resetState();const std::string scenario=argv[1];
 configureTimeService(true);assert(configCalls==1&&callback&&backendInterval==6UL*60*60*1000);
 if(scenario=="first-connection"){
  assert(!attemptTimeSync(false)&&lastTimeSyncAttempt==0);
  checkTimeSync();assert(!restarts&&!timeSynced);
  WiFi.state=WL_CONNECTED;++clockMs;checkTimeSync();
  assert(restarts==1&&!timeSynced&&lastTimeSyncSuccess==0);
  assert(std::string(timeSyncStatusCode())=="syncing");
  clockMs+=2000;reply();assert(!timeSynced); // only the loop changes app state.
  checkTimeSync();assert(timeSynced&&lastTimeSyncSuccess==clockMs&&restarts==1);
 }else if(scenario=="late-response-and-rtc"){
  validClock=true;WiFi.state=WL_CONNECTED;checkTimeSync();
  assert(!timeSynced&&lastTimeSyncSuccess==0); // retained RTC is not NTP proof.
  clockMs+=5000;checkTimeSync();assert(!timeSynced&&restarts==1);
  reply();checkTimeSync();const auto success=lastTimeSyncSuccess;assert(timeSynced);
  clockMs+=1000;checkTimeSync();assert(lastTimeSyncSuccess==success);
  reply();checkTimeSync();assert(lastTimeSyncSuccess==clockMs); // background resync.
  validClock=false;checkTimeSync();assert(!timeSynced);
 }else if(scenario=="retry-and-wrap"){
  clockMs=0xFFFFFF00;WiFi.state=WL_CONNECTED;checkTimeSync();assert(restarts==1);
  clockMs+=29999;checkTimeSync();assert(restarts==1&&!timeSynced);
  ++clockMs;checkTimeSync();assert(restarts==2); // 30 s, across millis wrap.
  clockMs+=2500;reply();checkTimeSync();assert(timeSynced);
  clockMs+=60000;checkTimeSync();assert(restarts==2); // no 30 s retries once synced.
  clockMs=lastTimeSyncAttempt+6UL*60*60*1000;checkTimeSync();assert(restarts==3);
 }else if(scenario=="reconnect"){
  WiFi.state=WL_CONNECTED;checkTimeSync();reply();checkTimeSync();
  WiFi.state=0;clockMs+=2000;checkTimeSync();
  WiFi.state=WL_CONNECTED;checkTimeSync();assert(restarts==1);
  clockMs+=13000;checkTimeSync();assert(restarts==2); // reconnect honored after rate limit.
  checkTimeSync();assert(restarts==2);
 }else if(scenario=="disable-enable"){
  WiFi.state=WL_CONNECTED;checkTimeSync();reply();
  timeSyncEnabled=false;configureTimeService(false);checkTimeSync();
  assert(!timeSynced&&!backendEnabled&&stops==1&&lastTimeSyncSuccess==0);
  assert(std::string(timeSyncStatusCode())=="disabled");
  const int count=restarts;clockMs+=60000;checkTimeSync();assert(restarts==count);
  assert(!attemptTimeSync(true));
  // A late callback while Off cannot be consumed as a fresh reply after enabling.
  reply();timeSyncEnabled=true;configureTimeService(true);checkTimeSync();assert(!timeSynced);
  assert(restarts==count+1);reply();checkTimeSync();assert(timeSynced);
 }else if(scenario=="disabled-at-boot"){
  resetState();timeSyncEnabled=false;configureTimeService(false);
  WiFi.state=WL_CONNECTED;checkTimeSync();
  assert(configCalls==0&&!backendInitialized&&!stops&&!restarts);
  timeSyncEnabled=true;configureTimeService(true);checkTimeSync();
  assert(configCalls==1&&restarts==1&&!timeSynced);reply();checkTimeSync();assert(timeSynced);
 }else if(scenario=="timezone"){
  assert(std::string(getenv("TZ"))==timeZoneRule(TIME_ZONE_CENTRAL_EUROPE));
  timeZoneMode=TIME_ZONE_NEW_ZEALAND;configureTimeService(true);
  assert(configCalls==1&&std::string(getenv("TZ"))==timeZoneRule(TIME_ZONE_NEW_ZEALAND));
  timeZoneMode=TIME_ZONE_CUSTOM;customTimeZoneRule="JST-9";configureTimeService(true);
  assert(configCalls==1&&std::string(getenv("TZ"))=="JST-9");
  timeZoneMode=TIME_ZONE_FIXED;timeOffsetMinutes=-210;configureTimeService(true);
  assert(std::string(getenv("TZ"))=="UTC+3:30");
 }else if(scenario=="backend-failure-and-invalid-reply"){
  WiFi.state=WL_CONNECTED;restartOk=false;checkTimeSync();assert(restarts==1&&!timeSynced);
  for(int i=0;i<100;++i){++clockMs;checkTimeSync();}assert(restarts==1);
  clockMs=lastTimeSyncAttempt+30000;restartOk=true;checkTimeSync();assert(restarts==2);
  validClock=true;timeval invalid{100,0};callback(&invalid);checkTimeSync();assert(!timeSynced);
  reply();checkTimeSync();assert(timeSynced);
 }else assert(false);
}
'''


class TimeSyncTests(unittest.TestCase):
    def test_manual_endpoint_reports_a_request_not_sync_completion(self):
        production = function((ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text(), 'httpActionTimeSync')
        code = r'''
#include "Arduino.h"
#define F(value) value
bool authorized=false,accepted=false;int requests=0;String response;
bool requireMutationAuth(){return authorized;}
bool attemptTimeSync(bool log,bool quick){assert(log&&quick);++requests;return accepted;}
void apiSendJSON(const String& json){response=json;}
''' + production + r'''
int main(){
 httpActionTimeSync();assert(!requests&&response.empty());
 authorized=true;httpActionTimeSync();assert(requests==1&&response.find("\"ok\":false")!=std::string::npos);
 accepted=true;httpActionTimeSync();assert(requests==2&&response.find("\"pending\":true")!=std::string::npos);
 assert(response.find("\"synced\":true")==std::string::npos);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text(STUBS['Arduino.h'])
            (tmp / 'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(tmp),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True, timeout=10)

    def test_actual_callback_and_scheduler(self):
        source = SOURCE.read_text()
        globals_ = source[source.index('// Time / NTP'):source.index('// mDNS')]
        production = '\n'.join(function(source, name) for name in (
            'onNtpTimeSync', 'configureTimeService', 'attemptTimeSync',
            'timeSyncStatusCode', 'checkTimeSync'))
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text(STUBS['Arduino.h'])
            (tmp / 'test.cpp').write_text(PREFIX + globals_ + production + HARNESS)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(tmp), '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            for scenario in ('first-connection', 'late-response-and-rtc', 'retry-and-wrap',
                             'reconnect', 'disable-enable', 'disabled-at-boot', 'timezone', 'backend-failure-and-invalid-reply'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(tmp / 'test'), scenario], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
