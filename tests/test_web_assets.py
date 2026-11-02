from webui_source import webui_source
"""Verify reproducible minified/gzip payloads and render the real OTA page states."""
import gzip
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / 'esp32-birdnet-mic'


class WebAssetsTests(unittest.TestCase):
    def test_actual_ota_partition_size_boundaries(self):
        source = webui_source(ROOT)
        start = source.index('static bool validateOtaImageSize(')
        end = source.index('static bool isSafeOtaUrl(', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <string>
using String = std::string;
#define F(text) text
#define FLASH_LAYOUT_TOOL 1
struct { uint32_t size=1310720; uint32_t getFreeSketchSpace(){return size;} } ESP;
''' + source[start:end] + r'''
int main() {
 String error;
 assert(!validateOtaImageSize(0,error));
 assert(validateOtaImageSize(1309200,error)); // Fits in the last sector.
 assert(validateOtaImageSize(ESP.size,error));
 assert(!validateOtaImageSize(ESP.size+1,error));
 assert(!validateOtaImageSize(4194304,error));
 assert(!validateOtaImageSize(8388608,error));
 ESP.size=0;assert(!validateOtaImageSize(100,error));
 ESP.size=3342336;assert(validateOtaImageSize(2000000,error));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'size.cpp', Path(temp) / 'size'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_embedded_bytes_equal_sources(self):
        for header, symbol, filename in [('WebUI_gz.h', 'WEBUI_INDEX_GZ', 'index.html'),
                                         ('OtaAssets_gz.h', 'OTA_CSS_GZ', 'ota.css'),
                                         ('OtaAssets_gz.h', 'OTA_JS_GZ', 'ota.js')]:
            text = (SKETCH / header).read_text()
            body = re.search(rf'{symbol}\[\].*?=\s*\{{(.*?)\}};', text, re.S).group(1)
            packed = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-f]{2})', body))
            length = int(re.search(rf'{symbol}_LEN = (\d+);', text).group(1))
            self.assertEqual(len(packed), length)
            raw = (SKETCH / 'webui' / filename).read_bytes()
            if filename.endswith(('.html', '.js', '.css')):
                raw = subprocess.check_output(['node', str(ROOT / 'tools/minify_webui.cjs'), filename.rsplit('.', 1)[-1]], input=raw)
            self.assertEqual(gzip.decompress(packed), raw)
            self.assertLess(len(packed), len(gzip.compress(raw, mtime=0)))

    def test_generation_is_reproducible(self):
        subprocess.run([str(SKETCH / 'tools/gen_webui_gzip_header.sh'), '--check'], check=True, timeout=30)

    def test_actual_ota_html_states_and_dynamic_escaping(self):
        source = webui_source(ROOT)
        escape = source[source.index('static String htmlEscape('):source.index('static void sendOtaPage(')]
        page = source[source.index('static void sendOtaPage('):source.index('static bool isLikelyMergedFirmwareName(')]
        code = r'''
#include <string>
#include <iostream>
struct String : std::string { using std::string::string; String(std::string s):std::string(s){} };
#define F(text) text
#define FLASH_LAYOUT_TOOL 1
bool checked=true, checking=false, available=false, fresh=true, prepared=true;
namespace flashlayout { bool otaAllowed(String& reason){ if(!prepared) reason="Please prepare your device first using the Prepare device button above."; return prepared; } }
String latest="1.28", error, url="http://firmware.test/firmware-app-c6.bin";
const char* FW_VERSION_STR="1.27";
const char* FW_BOARD_NAME_STR="XIAO ESP32-C6";
const char* FW_CHIP_FAMILY_STR="ESP32-C6";
struct { struct IP { String toString(){return "192.0.2.1";} }; IP localIP(){return {};} } WiFi;
String getDefaultOtaUrl(){return url;}
void getOtaVersionSnapshot(bool& c,unsigned long& at,String& v,String& e,bool& running){c=checked;at=0;v=latest;e=error;running=checking;}
bool otaVersionCacheFresh(bool,unsigned long,const String&){return fresh;}
bool startOtaVersionCheckAsync(){checking=true;fresh=true;return true;}
bool isNewerFirmwareAvailable(const String&){return available;}
struct { String body; void sendHeader(const char*,const char*){} void send(int,const char*,const String& html){body=html;} } web;
''' + escape + page + r'''
int main(){
 for(int state=0;state<7;++state){
  prepared=state!=6;
  checked=true; checking=state==0; available=state==2 || state==6; fresh=state!=5;
  error=state==1?"offline <test>":"";url=state==4?"":"http://firmware.test/firmware-app-c6.bin";
  sendOtaPage("<script>&",false);
  std::cout<<web.body<<'\n';
 }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = Path(temp) / 'ota.cpp', Path(temp) / 'ota'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
            pages = subprocess.check_output([str(binary)], text=True).splitlines()
        for page in pages:
            self.assertIn("href='/ota.css'", page)
            self.assertIn("src='/ota.js'", page)
            self.assertIn("id='ota-upload-form'", page)
            self.assertIn("id='update-space-prepare'", page)
            self.assertIn('&lt;script&gt;&amp;', page)
            self.assertNotIn('<style>', page)
            self.assertNotIn('<script>', page)
        self.assertIn("data-ota-checking='1'", pages[0])
        self.assertIn('offline &lt;test&gt;', pages[1])
        self.assertIn("id='ota-install-form'", pages[2])
        self.assertIn('Firmware is up to date.', pages[3])
        self.assertIn('Automatic update is unavailable', pages[4])
        self.assertIn("data-ota-checking='1'", pages[5])
        self.assertIn("data-ota-allowed='0'", pages[6])
        self.assertIn("<button type='submit' disabled>Download and install", pages[6])
        self.assertIn("application/octet-stream' required disabled", pages[6])
        self.assertIn('Please prepare your device first', pages[6])
        # Browser tests use pages rendered by this actual C++ handler.
        output = ROOT / 'tmp/ota-rendered-pages.json'
        if output.parent.is_dir():
            output.write_text(json.dumps(pages))
