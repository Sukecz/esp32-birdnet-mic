"""Run the production provisioning state machine with simulated Wi-Fi and NVS.

No router/device is changed. The real loop, credential commit and AP policy run
against a deterministic clock, including a wrap-around and failed NVS writes.
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
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <cassert>
class String:public std::string {
public:
 using std::string::string;
 String(const std::string& s):std::string(s){}
 String(int v):std::string(std::to_string(v)){}
 void setCharAt(size_t n,char c){at(n)=c;}
 void toCharArray(char* p,size_t n)const{strncpy(p,c_str(),n);p[n-1]=0;}
};
inline uint32_t clockMs=100;
inline uint32_t millis(){return clockMs;}
struct ESPMock {uint64_t getEfuseMac(){return 0x112233445566ULL;}};
inline ESPMock ESP;
struct IPAddress{IPAddress(int=0,int=0,int=0,int=0){}};
''',
    'Preferences.h': r'''
#pragma once
#include "Arduino.h"
inline std::map<std::string,std::vector<uint8_t>> nvs;
inline bool failWrite=false;
struct Preferences {
 std::string space;
 bool begin(const char* s,bool){space=s;return true;}
 bool isKey(const char* k){return nvs.count(space+"/"+k);}
 size_t getBytesLength(const char* k){return nvs[space+"/"+k].size();}
 size_t getBytes(const char* k,void* p,size_t n){auto& v=nvs[space+"/"+k];if(v.size()!=n)return 0;memcpy(p,v.data(),n);return n;}
 size_t putBytes(const char* k,const void* p,size_t n){if(failWrite)return 0;auto b=(const uint8_t*)p;nvs[space+"/"+k]={b,b+n};return n;}
 void end(){}
};
''',
    'WiFi.h': r'''
#pragma once
#include "Arduino.h"
constexpr int WIFI_OFF=0,WIFI_STA=1,WL_CONNECTED=3,WIFI_SCAN_RUNNING=-1;
using arduino_event_id_t=int;
constexpr int ARDUINO_EVENT_WIFI_STA_GOT_IP=1,ARDUINO_EVENT_WIFI_STA_DISCONNECTED=2;
struct WiFiMock {
 int statusCode=0,scanning=-2,clients=0,starts=0,stops=0,begins=0;
 bool ap=false,autoconnect=true,pinUsed=false;
 bool staleDisconnect=false,stationEnabled=true;
 void(*event)(int)=nullptr;
 void onEvent(void(*cb)(int)){event=cb;}
 String ssid,key,lastPass;
 int radioMode=1,offFailures=0;bool partialOffFailure=false;
 int getMode(){return radioMode;}
 bool mode(int m){if(!m&&offFailures){--offFailures;if(partialOffFailure){radioMode=0;ap=false;stationEnabled=false;statusCode=0;}return false;}radioMode=m;if(!m){ap=false;stationEnabled=false;statusCode=0;}return true;} void scanDelete(){scanning=-2;} void persistent(bool){} void setAutoReconnect(bool v){autoconnect=v;}
 int status(){return statusCode;} String SSID(){return ssid;}
 void enableSTA(bool v){stationEnabled=v;}
 void enableAP(bool){} void softAPConfig(IPAddress,IPAddress,IPAddress){}
 bool softAP(const char*,const char* pass){ap=true;++starts;key=pass?pass:"";return true;}
 IPAddress softAPIP(){return {};}
 void softAPdisconnect(bool){ap=false;++stops;}
 int softAPgetStationNum(){return clients;}
 void disconnect(bool){if(staleDisconnect)return;if(statusCode==WL_CONNECTED&&event)event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);statusCode=0;}
 void begin(const char* s,const char* p,int=0,const uint8_t* b=nullptr,bool=true){++begins;ssid=s;lastPass=p?p:"";pinUsed=b!=nullptr;}
 int scanComplete(){return scanning;}
};
inline WiFiMock WiFi;
''',
    'DNSServer.h': '#pragma once\n#include "Arduino.h"\nstruct DNSServer{void start(int,const char*,IPAddress){} void stop(){} void processNextRequest(){}};\n',
    'esp_system.h': '#pragma once\ninline uint32_t esp_random(){static uint32_t n=0x89123456;return n++;}\n',
    'esp_wifi.h': r'''
#pragma once
#include "Arduino.h"
constexpr int ESP_OK=0,WIFI_IF_STA=0,WIFI_STORAGE_RAM=0,WIFI_STORAGE_FLASH=1;
struct wifi_config_t {struct {uint8_t ssid[32],password[64];} sta;};
inline wifi_config_t legacy;
inline int esp_wifi_get_config(int,wifi_config_t* p){*p=legacy;return ESP_OK;}
inline int esp_wifi_set_storage(int){return ESP_OK;}
inline int esp_wifi_scan_stop(){WiFi.scanning=-2;return ESP_OK;}
inline int esp_wifi_set_config(int,const wifi_config_t* p){legacy=*p;return ESP_OK;}
''',
}
HARNESS = r'''
#include "DeviceSetup.cpp"
using namespace devicesetup;
bool pinnedEnabled=true;
uint8_t pin[6]={2,1,2,3,4,5};
const uint8_t* wifiApPinned(){return pinnedEnabled?pin:nullptr;}
bool wifiApSave(const uint8_t* p){if(failWrite)return false;pinnedEnabled=p;return true;}
void tick(uint32_t ms){clockMs+=ms;devicesetup::loop();}
void setLegacy(){strcpy((char*)legacy.sta.ssid,"Original");strcpy((char*)legacy.sta.password,"original-secret");}
Config stored(){Config c;auto& v=nvs["setup/config"];assert(v.size()==sizeof(c));memcpy(&c,v.data(),sizeof(c));assert(valid(c));return c;}
int main(int argc,char** argv){
 assert(argc==2);std::string scenario=argv[1];
 if(scenario=="wifi-off-failure"){
  setLegacy();begin(true);currentMode(true);WiFi.statusCode=WL_CONNECTED;
  const auto savedNvs=nvs;const int mode=WiFi.getMode();
  WiFi.offFailures=1;assert(requestWifiOff(250));tick(250);
  assert(!wifiSuspended()&&!wifiOffPending()&&*wifiOffError());
  assert(WiFi.getMode()==mode&&WiFi.status()==WL_CONNECTED&&nvs==savedNvs);
  assert(requestWifiOff(250)&&!*wifiOffError());tick(250);assert(wifiSuspended());
  begin(true);tick(15001);assert(apActive()); // direct recovery control must survive a failed stop too.
  WiFi.offFailures=1;WiFi.partialOffFailure=true;assert(requestWifiOff(250));tick(250);
  assert(!wifiSuspended()&&!wifiOffPending()&&*wifiOffError()&&apActive()&&WiFi.ap);
  assert(nvs==savedNvs);begin(true);assert(!*wifiOffError());
  WiFi.statusCode=WL_CONNECTED;WiFi.offFailures=1;const int begins=WiFi.begins;
  assert(requestWifiOff(250));tick(250);
  assert(!wifiSuspended()&&*wifiOffError()&&WiFi.getMode()==WIFI_STA&&WiFi.begins==begins+1);
  assert(nvs==savedNvs);return 0;
 }
 if(scenario=="wifi-off"){
  setLegacy();begin(true);currentMode(true);
  const auto savedNvs=nvs;
  clockMs=0xFFFFFF00;assert(requestWifiOff(500));assert(wifiOffPending());
  int before=WiFi.begins;reconnect();tick(499);assert(!wifiSuspended()&&WiFi.begins==before);
  tick(1);assert(wifiSuspended()&&!wifiOffPending()&&WiFi.radioMode==0&&!apActive());
  assert(!testCredentials("Home","password")&&!requestWifiOff(0));reconnect();
  for(int i=0;i<10;++i)tick(60000);
  assert(WiFi.radioMode==0&&WiFi.begins==before&&nvs==savedNvs);
  begin(true);assert(!wifiSuspended()&&!wifiOffPending());
  return 0;
 }
 if(scenario=="new"){
  begin(false);assert(!complete()&&!defaultRecording()&&apActive()&&WiFi.key.empty()&&!WiFi.stationEnabled);
  assert(apName().find("BirdNETMic-")==0&&apPassword().size()==8&&!WiFi.autoconnect);
  assert(stored().complete==0&&WiFi.begins==0);
  assert(save(true,true,true));currentMode(true);tick(500);
  assert(defaultRecording()&&standalone()&&WiFi.begins==0);
  WiFi.clients=1;tick(4000);assert(WiFi.key==apPassword()&&WiFi.key.size()==8&&apActive());
  currentMode(false);tick(500);assert(defaultRecording()); // Temporary Streaming keeps startup Recording.
 }else if(scenario=="legacy"){
  setLegacy();begin(true);assert(complete()&&defaultRecording()&&!standalone()&&!apActive());
  assert(savedSsid()=="Original"&&WiFi.pinUsed&&stored().password==std::string("original-secret"));
  tick(14999);assert(!apActive());tick(1);assert(apActive()&&WiFi.key.size()==8);
  WiFi.statusCode=WL_CONNECTED;WiFi.clients=1;tick(10);tick(65000);assert(apActive());
  WiFi.clients=0;tick(1);assert(!apActive());
  WiFi.statusCode=0;tick(15001);assert(apActive());
 }else if(scenario=="trial-failure"){
  setLegacy();begin(false);auto before=nvs["setup/config"];
  assert(testCredentials("New network","wrong-password"));assert(connectionBusy());
  assert(nvs["setup/config"]==before);tick(300);assert(WiFi.ssid=="New network"&&!WiFi.pinUsed);
  assert(!save(false,false,true));tick(25000);
  assert(std::string(connectionState())=="error"&&savedSsid()=="Original");
  assert(WiFi.ssid=="Original"&&WiFi.pinUsed&&nvs["setup/config"]==before);
  WiFi.statusCode=WL_CONNECTED;WiFi.clients=1;tick(70000);assert(apActive());
 }else if(scenario=="trial-success"){
  setLegacy();begin(false);auto before=nvs["setup/config"];
  assert(testCredentials("New network","new-password"));tick(300);
  WiFi.statusCode=WL_CONNECTED;WiFi.event(ARDUINO_EVENT_WIFI_STA_GOT_IP);tick(10);assert(std::string(connectionState())=="connected");
  assert(nvs["setup/config"]==before);failWrite=true;
  assert(!save(true,false,true)&&savedSsid()=="Original");failWrite=false;
  assert(save(true,false,true)&&savedSsid()=="New network"&&!pinnedEnabled);
  assert(stored().password==std::string("new-password")&&stored().recording);
  assert(std::string((char*)legacy.sta.ssid)=="Original"); // SDK flash untouched by trials.
 }else if(scenario=="same-ssid-proof"){
  setLegacy();begin(false);WiFi.statusCode=WL_CONNECTED;WiFi.staleDisconnect=true;
  assert(testCredentials("Original","incorrect-password"));tick(300);
  assert(std::string(connectionState())=="connecting");
  WiFi.event(ARDUINO_EVENT_WIFI_STA_GOT_IP);tick(10);assert(std::string(connectionState())=="connecting");
  WiFi.event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);WiFi.statusCode=0;tick(10);
  WiFi.event(ARDUINO_EVENT_WIFI_STA_GOT_IP);WiFi.statusCode=WL_CONNECTED;tick(10);
  assert(std::string(connectionState())=="connected");
 }else if(scenario=="uncommitted-trial-expiry"){
  setLegacy();begin(false);auto before=nvs["setup/config"];
  assert(testCredentials("New network","new-password"));tick(300);
  WiFi.event(ARDUINO_EVENT_WIFI_STA_GOT_IP);WiFi.statusCode=WL_CONNECTED;tick(10);
  assert(std::string(connectionState())=="connected");tick(300000);
  assert(std::string(connectionError())=="trial_expired"&&savedSsid()=="Original");
  assert(WiFi.ssid=="Original"&&nvs["setup/config"]==before);
 }else if(scenario=="scan-and-wrap"){
  setLegacy();clockMs=0xFFFFF000;begin(false);WiFi.scanning=WIFI_SCAN_RUNNING;
  assert(!testCredentials("New","password"));int calls=WiFi.begins;
  tick(31000);assert(apActive()&&WiFi.begins==calls);WiFi.scanning=-2;
  tick(1);assert(WiFi.begins==calls+1); // retry after scan, including timer wrap.
  for(const auto& bad: {std::string(""),std::string(33,'a')})assert(!testCredentials(bad,"password"));
  assert(!testCredentials("Name","short"));
 }else if(scenario=="migration-recording-no-wifi"){
  nvs["recording/settings"]={1};begin(true);assert(complete()&&defaultRecording());
  tick(15000);assert(apActive()); // existing offline Recording does not re-enter welcome.
 }else if(scenario=="legacy-key"){
  LegacyConfig previous;previous.complete=1;strcpy(previous.ssid,"Original");strcpy(previous.apKey,"ABCDEF1234567890");
  auto* bytes=(uint8_t*)&previous;nvs["setup/config"]={bytes,bytes+sizeof(previous)};
  begin(false);assert(apPassword()=="ABCDEF1234567890"&&complete());
  failWrite=true;assert(!shortenApPassword()&&apPassword().size()==16);failWrite=false;
  tick(15000);WiFi.clients=1;assert(!shortenApPassword()&&apPassword().size()==16);WiFi.clients=0;
  assert(shortenApPassword()&&apPassword().size()==8&&savedSsid()=="Original"&&complete());
  for(char c:apPassword())assert(strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789",c));
  auto saved=nvs["setup/config"];assert(shortenApPassword()&&nvs["setup/config"]==saved);
 }else if(scenario=="unfinished-key"){
  LegacyConfig previous;strcpy(previous.apKey,"ABCDEF1234567890");
  auto* bytes=(uint8_t*)&previous;nvs["setup/config"]={bytes,bytes+sizeof(previous)};
  begin(false);assert(!complete()&&apPassword().size()==8&&apActive()&&WiFi.key.empty());
  assert(strlen(stored().apKey)==8);
 }else if(scenario=="custom-key"){
  setLegacy();begin(true);const Config original=stored();auto before=nvs;
  for(const auto& bad:{std::string("short"),std::string(64,'a'),std::string("accent-\xC4\x8D"),std::string("abcd\n1234"),std::string("abc\0defghi",10)}){
   assert(!setApPassword(String(bad))&&nvs==before);
  }
  const String custom=" Long custom password + symbols & spaces! ";
  failWrite=true;assert(!setApPassword(custom)&&nvs==before);failWrite=false;
  assert(testCredentials("Other","password"));assert(!setApPassword(custom));tick(300);tick(26000);
  // An AP client receives the acknowledgement before the delayed rekey.
  tick(15001);WiFi.clients=1;const int starts=WiFi.starts;const String previous=WiFi.key;
  assert(setApPassword(custom)&&apPassword()==custom);
  assert(WiFi.key==previous&&WiFi.starts==starts);tick(1499);assert(WiFi.key==previous);
  tick(1);assert(WiFi.key==custom&&WiFi.starts==starts+1);
  assert(stored().password==std::string(original.password)&&stored().recording==original.recording);
  before=nvs;failWrite=true;assert(setApPassword(custom)&&nvs==before);failWrite=false;
  begin(true);assert(apPassword()==custom&&complete());
  assert(setApPassword(String(std::string(63,'x'))));begin(true);assert(apPassword().size()==63);
  assert(setApPassword("12345678"));begin(true);assert(apPassword()=="12345678");
 }else if(scenario=="migration-write-failure"){
  LegacyConfig old;old.complete=1;old.recording=1;old.batteryEnabled=1;
  strcpy(old.ssid,"Keep network");strcpy(old.password,"keep password");strcpy(old.apKey,"OLDKEY12");
  auto* bytes=(uint8_t*)&old;nvs["setup/config"]={bytes,bytes+sizeof(old)};const auto before=nvs;
  failWrite=true;begin(true);assert(complete()&&defaultRecording()&&batteryMonitoring());
  assert(apPassword()=="OLDKEY12"&&savedSsid()=="Keep network"&&nvs==before);
  failWrite=false;assert(setApPassword("OLDKEY12"));assert(stored().password==std::string("keep password"));
  begin(true);assert(apPassword()=="OLDKEY12"&&savedSsid()=="Keep network");
 }else if(scenario=="battery"){
  setLegacy();begin(false);assert(!batteryMonitoring()&&stored().batteryEnabled==0);
  const auto before=nvs["setup/config"];
  failWrite=true;assert(!save(false,false,true,1));assert(!batteryMonitoring()&&nvs["setup/config"]==before);
  failWrite=false;assert(save(false,false,true,1)&&batteryMonitoring());
  assert(savedSsid()=="Original"&&stored().password==std::string("original-secret"));
  const auto enabled=nvs["setup/config"];failWrite=true;
  assert(save(false,false,true,1)&&nvs["setup/config"]==enabled); // unchanged save does no write.
  assert(save(false,false,true)&&batteryMonitoring()); // older client preserves the choice.
  assert(!save(false,false,true,2)&&batteryMonitoring());failWrite=false;
  begin(false);assert(batteryMonitoring()); // persisted choice survives initialization.
  const auto unchanged=nvs["setup/config"];
  failWrite=true;assert(setBatteryMonitoring(true)&&nvs["setup/config"]==unchanged);
  assert(!setBatteryMonitoring(false)&&batteryMonitoring());failWrite=false;
  assert(setBatteryMonitoring(false)&&!batteryMonitoring()&&savedSsid()=="Original");
  assert(setBatteryMonitoring(true)&&batteryMonitoring());
  assert(reset(false)&&batteryMonitoring()); // Wi-Fi reset keeps the setting.
  assert(reset(true)&&!batteryMonitoring());
 }else if(scenario=="reset"){
  setLegacy();begin(true);assert(reset(false));assert(!complete()&&defaultRecording()&&savedSsid().empty());
  assert(!legacy.sta.ssid[0]);assert(stored().complete==0);assert(reset(true)&&!defaultRecording());
 }else assert(false);
}
'''


class DeviceSetupTests(unittest.TestCase):
    def test_real_scan_handlers_include_all_networks_before_configuration(self):
        web = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()

        def function(signature):
            start = web.index(signature + ' {')
            end, depth = web.index('{', start) + 1, 1
            while depth:
                depth += (web[end] == '{') - (web[end] == '}')
                end += 1
            return web[start:end]

        code = r'''
#include "Arduino.h"
#include <algorithm>
#define F(s) s
constexpr int WIFI_SCAN_RUNNING=-1,WIFI_AUTH_OPEN=0,WL_CONNECTED=3;
bool auth=true,busy=false,wifiApScanRequested=false,wifiApScanAll=false,wifiApScanQueued=false;
unsigned long wifiReconnectAt=0,wifiApScanStarted=0;
String savedSsid,response;
struct {String path="/api/setup/scan";String uri(){return path;}} web;
bool requireMutationAuth(){return auth;}
void apiSendJSON(const String& s){response=s;}
String jsonEscape(const String& s){return s;}
String wifiApSsid(){return savedSsid;}
namespace devicesetup {bool connectionBusy(){return busy;} bool apActive(){return true;}}
struct {
 int count=-2,calls=0,dwell=0;String filter;
 int scanComplete(){return count;}
 void scanDelete(){count=-2;}
 bool stationEnabled=true;
 int status(){return 0;} void enableSTA(bool v){stationEnabled=v;}
 int scanNetworks(bool async,bool hidden,bool passive=false,int ms=300,int channel=0,const char* ssid=nullptr){
  assert(!passive&&!channel);++calls;dwell=ms;filter=ssid?ssid:"";
  if(!ssid){assert(!async&&!hidden);stationEnabled=true;return count=3;}
  assert(async&&hidden);return count=WIFI_SCAN_RUNNING;
 }
 String SSID(int i){return i==0?"Neighbour":i==1?"Home":"";}
 String BSSIDstr(int i){return i==0?"02:00:00:00:00:01":"02:00:00:00:00:02";}
 int RSSI(int i){return i==0?-70:-50;}int encryptionType(int i){return i==0?0:3;}
} WiFi;
''' + function('static void httpWifiApScanStart()') + '\n' + function('static void wifiApScanLoop()') + '\n' + function('static void httpWifiApScanResults()') + r'''
int main(){
 auth=false;httpWifiApScanStart();assert(!WiFi.calls);auth=true;
 httpWifiApScanStart();assert(WiFi.calls==0&&wifiApScanQueued);
 httpWifiApScanResults();assert(response.find("\"scanning\":true")!=std::string::npos);
 wifiApScanLoop();assert(WiFi.calls==0);
 httpWifiApScanStart();assert(response.find("busy")!=std::string::npos);
 clockMs+=300;wifiApScanLoop();assert(WiFi.calls==1&&WiFi.dwell==300&&WiFi.filter.empty());
 assert(!WiFi.stationEnabled);
 WiFi.count=3;httpWifiApScanResults();assert(response.find("Neighbour")!=std::string::npos&&response.find("Home")!=std::string::npos);
 assert(response.find("Home")<response.find("Neighbour"));
 web.path="/api/wifi/scan";httpWifiApScanStart();assert(WiFi.calls==1&&response.find("no_ssid")!=std::string::npos);
 savedSsid="Home";httpWifiApScanStart();clockMs+=300;wifiApScanLoop();assert(WiFi.calls==2&&WiFi.dwell==120&&WiFi.filter=="Home");
 WiFi.count=3;httpWifiApScanResults();assert(response.find("Neighbour")==std::string::npos&&response.find("Home")!=std::string::npos);
 clockMs+=60001;httpWifiApScanResults();assert(response.find("scan_expired")!=std::string::npos);
 busy=true;WiFi.count=-2;httpWifiApScanStart();assert(WiFi.calls==2&&response.find("busy")!=std::string::npos);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text(STUBS['Arduino.h'])
            (tmp / 'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(tmp), str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True, timeout=10)

    def test_real_setup_endpoints_validate_before_writing(self):
        web = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()

        def function(signature):
            start = web.index(signature + ' {')
            end, depth = web.index('{', start) + 1, 1
            while depth:
                depth += (web[end] == '{') - (web[end] == '}')
                end += 1
            return web[start:end]

        code = r'''
#include "Arduino.h"
#include "S3Boards.h"
#include <cerrno>
#include <cstdlib>
#define F(s) s
namespace fieldpower { struct Config{bool guardEnabled=false;};inline Config settings(){return {};} }
bool auth=true,boardSupport=true,antennaSupport=false,saveOk=true,applyOk=true;
int mutations=0,reboots=0,modeChanges=0,lastCode=200;
uint8_t s3BoardVariant=0,micFormat=0;
uint32_t currentSampleRate=32000,minAcceptableRate=30;
uint16_t highpassCutoffHz=12000;
bool autoThresholdEnabled=true;
constexpr uint8_t MIC_FORMAT_PDM=2;
constexpr uint32_t PDM_SAMPLE_RATE=48000;
s3boards::Config persistedBoard{0,0,32000,30,12000};
std::vector<std::string> operations;
struct {std::map<std::string,String> args;String arg(const char* n){return args[n];}void send(int code,const char*,const char*){lastCode=code;}} web;
bool requireMutationAuth(){return auth;}
void apiSendJSON(const String&){}
namespace recording {
 bool pending=false,active=false,working=false;int checks=0;
 bool resumePending(){return pending;}bool mode(){return active;}bool busy(){return working;}
 bool checkCard(bool supported,uint8_t){++checks;working=true;return supported;}
}
namespace recordingpins {struct Pins{bool supported;};Pins resolve(uint8_t v){return {v!=2};}}
namespace devicesetup {
 bool done=false,busy=false;String ssid="",state="idle";bool startup=false,direct=false;
 bool complete(){return done;}bool connectionBusy(){return busy;}
 String savedSsid(){return ssid;}const char* connectionState(){return state.c_str();}
 bool batteryOn=false;
 bool save(bool rec,bool ap,bool finish,int8_t battery=-1){++mutations;operations.push_back("setup");if(!saveOk)return false;startup=rec;direct=ap;done=finish;if(battery>=0)batteryOn=battery;return true;}
 void currentMode(bool){}
 bool testCredentials(const String&,const String&){++mutations;return true;}
}
namespace antenna {bool supported(){return antennaSupport;}bool external(){return true;}bool save(bool){++mutations;return true;}}
namespace battery {struct Pins{bool supported;};Pins pins(uint8_t v){return {v!=2};}}
bool s3BoardSelectionSupported(){return boardSupport;}
void stopAudioProducer(){++mutations;}
bool restartI2S(){return applyOk;}
void updateS3BoardIdentity(){}
uint16_t normalizeHighpassCutoff(uint32_t rate,uint16_t cutoff){return cutoff<rate/2?cutoff:rate/2-1;}
uint32_t computeRecommendedMinRate(){return currentSampleRate/1024;}
bool recordingHardwareSupported(){return true;}
void simplePrintln(const char*){}
''' + '\n'.join(self.sketch_function(sig) for sig in (
    's3boards::Config getS3BoardConfig()',
    'bool restoreS3BoardConfig(const s3boards::Config& saved)',
    'bool applyS3BoardConfig(uint8_t variant)')) + r'''
void saveAudioSettings(){++mutations;operations.push_back("board");persistedBoard=getS3BoardConfig();}
bool setDeviceMode(bool rec){++modeChanges;recording::active=rec;return true;}
void scheduleReboot(bool,uint32_t){++reboots;}
''' + function('static bool parseUInt32Strict(const String& input, uint32_t& out)') + '\n' + function('static void httpSetupSave()') + '\n' + function('static void httpSetupConnect()') + r'''
int main(){
 web.args={{"mode","recording"},{"connection","direct"},{"variant","1"},{"antenna","external"}};
 auth=false;httpSetupSave();httpSetupConnect();assert(!mutations);auth=true;
 for(const char* bad:{"-1","3","256","1x","","999999999999999999999"}){
  web.args["variant"]=bad;httpSetupSave();assert(!mutations&&lastCode==400);
 }
 web.args["variant"]="2";httpSetupSave();assert(!mutations); // Atom has no SD interface.
 web.args["variant"]="1";recording::pending=true;httpSetupSave();assert(!mutations);recording::pending=false;
 for(const char* bad:{"2","-1","true","on"}){web.args["battery"]=bad;httpSetupSave();assert(!mutations&&lastCode==400);}
 web.args["battery"]="1";web.args["variant"]="2";web.args["mode"]="streaming";web.args["connection"]="home";
 httpSetupSave();assert(!mutations&&lastCode==400);
 web.args["variant"]="1";web.args["mode"]="recording";
 web.args["connection"]="home";httpSetupSave();assert(!mutations&&lastCode==409);
 web.args["connection"]="direct";applyOk=false;httpSetupSave();assert(!devicesetup::done&&!modeChanges);applyOk=true;
 mutations=0;saveOk=false;httpSetupSave();assert(!devicesetup::done&&s3BoardVariant==0&&!modeChanges);
 assert(micFormat==0&&currentSampleRate==32000&&highpassCutoffHz==12000&&minAcceptableRate==30);
 assert(persistedBoard.variant==0&&persistedBoard.format==0&&persistedBoard.rate==32000);
 // The same failed commit in Recording must roll back before SD can become busy.
 recording::active=true;httpSetupSave();
 assert(s3BoardVariant==0&&micFormat==0&&currentSampleRate==32000&&!recording::working&&!recording::checks);
 assert(persistedBoard.variant==0&&persistedBoard.format==0&&persistedBoard.rate==32000);
 recording::active=false;
 saveOk=true;operations.clear();httpSetupSave();
 assert(devicesetup::done&&devicesetup::startup&&devicesetup::direct&&recording::active&&modeChanges==1&&devicesetup::batteryOn);
 assert(operations==std::vector<std::string>({"board","setup"}));
 assert(recording::checks==1&&recording::working);recording::working=false;
 web.args["mode"]="streaming";web.args["connection"]="home";web.args["allow_offline"]="1";
 httpSetupSave();assert(!devicesetup::startup&&recording::active&&modeChanges==1); // default != current.
 int before=mutations;web.args["connection"]="direct";httpSetupSave();assert(mutations==before);
 boardSupport=false;antennaSupport=true;web.args["connection"]="home";web.args["antenna"]="bad";
 httpSetupSave();assert(mutations==before);web.args["antenna"]="external";httpSetupSave();assert(!reboots);
 before=mutations;web.args["ssid"]=String(std::string("ab\0cd",5));httpSetupConnect();assert(mutations==before);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text(STUBS['Arduino.h'])
            (tmp / 'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(tmp), '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True, timeout=10)

    @staticmethod
    def sketch_function(signature):
        source = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
        start = source.index(signature + ' {')
        end, depth = source.index('{', start) + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]

    def test_ap_password_endpoint(self):
        source = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()
        handler = 'static void httpSetupAccess() {' + source.split('static void httpSetupAccess() {', 1)[1].split('\nstatic void httpStatus()', 1)[0]
        code = r'''
#include "Arduino.h"
#include "SetupConfig.h"
bool auth=true,active=false,pending=false,allowed=true,writeOk=true;int writes=0,lastCode=200;
String current="OLDKEY12",reply;
struct {std::map<std::string,String> args;bool hasArg(const char* n){return args.count(n);}String arg(const char* n){return args[n];}void send(int code,const char*,const char* body){lastCode=code;reply=body;}} web;
bool requireMutationAuth(){return auth;}
String jsonEscape(const String& s){return s;}
void apiSendJSON(const String& s){lastCode=200;reply=s;}
namespace recording {bool busy(){return active;}bool resumePending(){return pending;}}
namespace devicesetup {
 bool canChangeApPassword(){return allowed;}
 String apPassword(){return current;}
 bool setApPassword(const String& s){++writes;if(!writeOk)return false;current=s;return true;}
 bool shortenApPassword(){++writes;return writeOk;}
}
''' + handler + r'''
int main(){
 auth=false;web.args={{"password","Test secret"}};httpSetupAccess();assert(!writes);auth=true;
 for(const auto& bad:{std::string(""),std::string("short"),std::string(64,'a'),std::string("line\nbreak"),std::string("accent-\xC4\x8D"),std::string("a\0b12345678",11)}){
  web.args={{"password",String(bad)}};httpSetupAccess();assert(!writes&&lastCode==400);
 }
 web.args={{"password","Valid & key "},{"shorten","1"}};httpSetupAccess();assert(!writes&&lastCode==400);
 web.args={{"password","Valid & key "}};
 active=true;httpSetupAccess();assert(!writes&&lastCode==409);active=false;
 pending=true;httpSetupAccess();assert(!writes&&lastCode==409);pending=false;
 allowed=false;httpSetupAccess();assert(!writes&&lastCode==409);allowed=true;
 writeOk=false;httpSetupAccess();assert(writes==1&&lastCode==503&&current=="OLDKEY12");
 writeOk=true;httpSetupAccess();assert(writes==2&&lastCode==200&&current=="Valid & key ");
 web.args.clear();active=true;pending=true;httpSetupAccess();assert(writes==2&&lastCode==200);
 assert(reply.find("Valid & key ")!=std::string::npos);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'Arduino.h').write_text(STUBS['Arduino.h'])
            (tmp / 'test.cpp').write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(tmp), '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True, timeout=10)

    def test_production_state_machine(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name, content in STUBS.items():
                (tmp / name).write_text(content)
            (tmp / 'test.cpp').write_text(HARNESS)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(tmp), '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
            for scenario in ('wifi-off', 'wifi-off-failure', 'new', 'legacy', 'trial-failure', 'trial-success',
                             'scan-and-wrap', 'same-ssid-proof', 'uncommitted-trial-expiry', 'migration-recording-no-wifi', 'legacy-key', 'unfinished-key', 'custom-key', 'migration-write-failure', 'battery', 'reset'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(tmp / 'test'), scenario], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
