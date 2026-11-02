from webui_source import webui_source
from validation_fixture import uint_parser
"""Run the real grouped settings handlers: validation must precede every write."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsGroupsTests(unittest.TestCase):
    def test_actual_group_handlers(self):
        source = webui_source(ROOT)
        parse = uint_parser()
        handlers = source[source.index('// Validate the whole group'):source.index('static void httpSet()')]
        code = r'''
#include <string>
#include <map>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
struct String : std::string {
 using std::string::string;
 String(std::string s):std::string(s){}
 void trim(){auto a=find_first_not_of(" \r\n\t"),b=find_last_not_of(" \r\n\t");*this=a==npos?"":substr(a,b-a+1);}
};
#define F(s) s
struct Web {
 std::map<std::string,String> args;
 String arg(const char* k){return args[k];}
 bool hasArg(const char* k){return args.count(k);}
} web;
bool authorized=true,streamScheduleEnabled=false,deepSleepScheduleEnabled=false,mqttEnabled=false;
uint16_t streamScheduleStartMin=300,streamScheduleStopMin=1200,mqttPort=1883;
String mqttHost="old.example",mqttUser="old-user",mqttPassword="old-secret",reply;
int saves=0,reconnects=0;
bool requireMutationAuth(){return authorized;}
void apiSendJSON(const String& s){reply=s;}
void saveAudioSettings(){++saves;}
void mqttRequestReconnect(bool){++reconnects;}
void webui_pushLog(const String& s){assert(s.find("secret")==String::npos);}
''' + parse + handlers + r'''
int main(){
 web.args={{"enabled","on"},{"start","1320"},{"stop","360"},{"sleep","on"}};
 authorized=false;httpSaveSchedule();assert(!saves&&!streamScheduleEnabled);authorized=true;
 httpSaveSchedule();assert(saves==1&&streamScheduleEnabled&&deepSleepScheduleEnabled);
 assert(streamScheduleStartMin==1320&&streamScheduleStopMin==360);
 for(const auto& bad:{"1320","1440","-1","12x","","9999999999999999999999"}){
  web.args["stop"]=bad;httpSaveSchedule();assert(saves==1&&streamScheduleStopMin==360);
  assert(reply.find("false")!=String::npos);
 }
 web.args["stop"]="360";web.args["enabled"]="off";httpSaveSchedule();assert(saves==1);
 web.args["sleep"]="off";httpSaveSchedule();assert(saves==2&&!streamScheduleEnabled&&!deepSleepScheduleEnabled);
 web.args={{"enabled","on"},{"host","new.example"},{"port","1884"},{"user","new-user"},{"password",""},{"clear_password","off"}};
 authorized=false;httpSaveMqtt();assert(saves==2&&mqttHost=="old.example");authorized=true;
 for(const auto& bad:{"0","65536","4294967297","1883x","-1","","99999999999999999999"}){
  web.args["port"]=bad;httpSaveMqtt();assert(saves==2&&mqttHost=="old.example"&&mqttPassword=="old-secret");
 }
 web.args["port"]="1884";httpSaveMqtt();assert(saves==3&&reconnects==1&&mqttEnabled&&mqttHost=="new.example"&&mqttPassword=="old-secret");
 web.args["password"]="  new-secret  ";httpSaveMqtt();assert(saves==4&&reconnects==2&&mqttPassword=="  new-secret  ");
 web.args["clear_password"]="on";httpSaveMqtt();assert(saves==4&&reconnects==2);
 web.args["password"]="";httpSaveMqtt();assert(saves==5&&mqttPassword.empty());
 web.args["host"]="";httpSaveMqtt();assert(saves==5&&mqttHost=="new.example");
 web.args["enabled"]="off";httpSaveMqtt();assert(saves==6&&!mqttEnabled);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'groups.cpp', Path(temp) / 'groups'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
