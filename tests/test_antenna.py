from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class AntennaTests(unittest.TestCase):
    def test_gpio_selection_defaults_persistence_and_failed_writes(self):
        for board in ('XIAO_ESP32C3','XIAO_ESP32S3','XIAO_ESP32C5','XIAO_ESP32C6'):
            with self.subTest(board=board),tempfile.TemporaryDirectory() as directory:
                binary = Path(directory)/'test'
                subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-DARDUINO_'+board,
                                '-I',str(ROOT/'tests/antenna_stubs'),str(ROOT/'tests/antenna_test.cpp'),
                                str(ROOT/'esp32-birdnet-mic/Antenna.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)
    def test_actual_http_validation_and_deferred_reboot(self):
        source=(ROOT/'esp32-birdnet-mic/WebUI.cpp').read_text()
        start=source.index('static void httpSaveAntenna() {')
        end=source.index('{',start)+1
        depth=1
        while depth:
            depth+=(source[end]=='{')-(source[end]=='}')
            end+=1
        code=r'''
#include <cassert>
#include <map>
#include <string>
using String=std::string;
#define F(s) s
bool authorized=true,saveOk=true,supported=true,activeExternal=true;
int writes=0,reboots=0;String response;
namespace antenna {
 bool supported(){return ::supported;}
 bool external(){return activeExternal;}
 bool save(bool){++writes;return saveOk;}
}
struct {std::map<String,String> args;int code=0;String arg(const char* key){return args[key];}void send(int c,const char*,const char* text){code=c;response=text;}} web;
bool requireMutationAuth(){return authorized;}
void apiSendJSON(const String& text){web.code=200;response=text;}
void scheduleReboot(bool factory,unsigned delay){assert(!factory&&delay==800);assert(response.find("true")!=String::npos);++reboots;}
''' + source[start:end] + r'''
int main(){
 authorized=false;httpSaveAntenna();assert(writes==0&&reboots==0);
 authorized=true;supported=false;web.args["antenna"]="internal";httpSaveAntenna();assert(web.code==400&&writes==0);
 supported=true;web.args["antenna"]="auto";httpSaveAntenna();assert(web.code==400&&writes==0);
 web.args["antenna"]="external";httpSaveAntenna();assert(writes==1&&reboots==0&&response.find("false")!=String::npos);
 web.args["antenna"]="internal";saveOk=false;httpSaveAntenna();assert(reboots==0&&response.find("antenna_save_failed")!=String::npos);
 saveOk=true;httpSaveAntenna();assert(reboots==1&&activeExternal); // Handler does not switch RF before responding/restarting.
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory);(tmp/'test.cpp').write_text(code)
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True)
if __name__ == '__main__': unittest.main()
