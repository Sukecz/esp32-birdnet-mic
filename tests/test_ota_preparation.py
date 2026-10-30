"""Execute production OTA handlers; block before flash and preserve the old bridge."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WEB = (ROOT / 'esp32-birdnet-mic/WebUI.cpp').read_text()
SKETCH = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class OtaPreparationTests(unittest.TestCase):
    def native(self, source):
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', '-I', str(ROOT/'tests/flash_layout_stubs'),
                            str(cpp), '-lcrypto', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_initial_old_upgrade_then_new_gate_and_both_http_endpoints(self):
        prefix = r'''
#include "Platform.h"
namespace recording {bool busy(){return false;}}
bool auth=true,allowed=false;
int starts=0,writes=0,installs=0,checks=0,pauses=0,reboots=0,httpCode=0;
String message;
namespace flashlayout {bool busy(){return false;} bool otaAllowed(String& error){++checks;if(!allowed)error="Please prepare your device first using the Prepare device button above.";return allowed;}}
constexpr size_t UPDATE_SIZE_UNKNOWN=0xFFFFFFFF;
constexpr int UPLOAD_FILE_START=1,UPLOAD_FILE_WRITE=2,UPLOAD_FILE_END=3,UPLOAD_FILE_ABORTED=4;
struct HTTPUpload {int status=1;String filename="firmware-app-c6.bin";uint8_t buf[16]={};size_t currentSize=16,totalSize=1295000;} upload;
struct {HTTPUpload& upload(){return ::upload;}void sendHeader(const char*,const char*){}} web;
struct {
 bool begin(size_t){++starts;return true;}
 size_t write(uint8_t*,size_t n){++writes;return n;}
 void abort(){}const char* errorString(){return "write error";}
 bool end(bool=true){return true;}bool isFinished(){return true;}
} Update;
bool otaUploadOk=false,otaUploadBlocked=false;
String otaUploadError;
const char* FW_VERSION_STR="1.35";
bool requireMutationAuth(){return auth;}
bool hasMutationAuth(){return auth;}
void beginOtaUpdate(){++pauses;}
void endOtaUpdate(bool){}
void webui_pushLog(const String&){}
void scheduleReboot(bool,uint32_t){++reboots;}
void sendOtaPage(const String& text,bool,int code=200){message=text;httpCode=code;}
bool fetchLatestOtaVersion(bool,String& latest,String&){latest="1.36";return true;}
bool isNewerFirmwareAvailable(const String&){return true;}
String getDefaultOtaUrl(){return "http://esp32mic.msmeteo.cz/firmware-app-c6.bin";}
bool installOtaFromUrl(const String&,String&){++installs;return true;}
bool isLikelyMergedFirmwareName(String){return false;}
'''
        handlers = '\n'.join(function(WEB, sig) for sig in (
            'static bool validateOtaImageSize(', 'static void httpOtaInstall()',
            'static void httpOtaUploadDone()', 'static void httpOtaUploadChunk()'))
        legacy = (ROOT/'tests/fixtures/ota_v1_34_handlers.cpp').read_text()
        self.native(prefix + handlers + '\n' + legacy + r'''
void reset(){auth=true;allowed=false;starts=writes=installs=checks=pauses=reboots=httpCode=0;message="";otaUploadOk=otaUploadBlocked=false;otaUploadError="";}
int main(){
 // The published v1.34 updater does not call the preparation policy at all.
 // It can install the fitting bridge image before any Prepare tool is present.
 reset();legacyhttpOtaInstall();assert(installs==1 && checks==0 && reboots==1);
 reset();
 for(int step:{UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END}){upload.status=step;legacyhttpOtaUploadChunk();}
 legacyhttpOtaUploadDone();assert(starts==1 && writes==1 && otaUploadOk && reboots==1 && checks==0);
 // Once the tool-bearing firmware runs, even a small next image is blocked.
 reset();httpOtaInstall();assert(checks==1 && !installs && !starts && !pauses && !reboots && httpCode==409);
 assert(message.find("Prepare device")!=std::string::npos);
 reset();
 for(int step:{UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END}){upload.status=step;httpOtaUploadChunk();}
 httpOtaUploadDone();assert(otaUploadBlocked && !starts && !writes && !pauses && !reboots && httpCode==409);
 assert(message.find("Prepare device")!=std::string::npos);
 // CSRF validation still runs before readiness or any mutation.
 reset();auth=false;httpOtaInstall();assert(!checks && !installs);
 // Verified preparation enables both paths again, without a version bypass.
 reset();allowed=true;httpOtaInstall();assert(installs==1 && reboots==1);
 reset();allowed=true;
 for(int step:{UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END}){upload.status=step;httpOtaUploadChunk();}
 httpOtaUploadDone();assert(starts==1 && writes==1 && otaUploadOk && reboots==1);
}
''')

    def test_low_level_stream_rechecks_before_update_begin(self):
        prefix = r'''
#include "Platform.h"
#define yield() ((void)0)
bool allowed=false;
int starts=0,writes=0;
namespace flashlayout {bool otaAllowed(String& reason){reason="Prepare device";return allowed;}}
struct NetworkClient {
 int remaining=4;
 size_t available(){return remaining;}
 bool connected(){return true;}
 int readBytes(uint8_t* bytes,size_t n){memset(bytes,0,n);remaining-=n;return n;}
};
struct {
 bool begin(size_t){++starts;return true;}
 size_t write(uint8_t*,size_t n){++writes;return n;}
 void abort(){}const char* errorString(){return "write error";}
 bool end(){return true;}bool isFinished(){return true;}
} Update;
bool validateOtaImageSize(size_t,String&){return true;}
'''
        self.native(prefix + function(WEB,'static bool streamUpdate(') + r'''
int main(){String error;NetworkClient stream;assert(!streamUpdate(stream,4,error));assert(!starts && !writes && stream.remaining==4);allowed=true;assert(streamUpdate(stream,4,error));assert(starts==1 && writes==1);}
''')

    def test_arduino_ota_not_started_before_preparation(self):
        self.native(r'''
#include "Platform.h"
bool allowed=false;int begins=0;
namespace flashlayout {bool otaAllowed(String&){return allowed;}}
String mdnsHostname="mic";
using ota_error_t=int;
void simplePrintln(const String&){}
void beginOtaUpdate(){}
void endOtaUpdate(bool){}
struct {
 void setHostname(const char*){}
 template<class T>void onStart(T){}
 template<class T>void onError(T){}
 void begin(){++begins;}
} ArduinoOTA;
''' + function(SKETCH,'void setupOTA()') + r'''
int main(){setupOTA();assert(!begins);allowed=true;setupOTA();assert(begins==1);}
''')
