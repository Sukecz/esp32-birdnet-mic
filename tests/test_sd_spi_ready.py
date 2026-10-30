"""Regress stable-ready SD handling against the actual pinned Arduino SD source."""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare_sd_library', ROOT/'tools/prepare_sd_library.py')
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)
PREFIX = '\n#include <cassert>\n#include <cstdint>\n#include <vector>\nuint32_t nowMs=0;\nuint32_t millis(){return nowMs;}\n#define log_w(...) ((void)0)\n#define log_e(...) ((void)0)\n#define HIGH 1\n#define LOW 0\nbool selected=false;int releaseClocks=0;\nvoid digitalWrite(int,int value){selected=value==LOW;}\nstruct Spi {\n std::vector<uint8_t> replies;size_t at=0;\n uint8_t transfer(uint8_t value){assert(value==255);++nowMs;if(!selected)++releaseClocks;return at<replies.size()?replies[at++]:uint8_t(0);}\n void write(uint8_t){}\n};\nstruct ardu_sdcard_t{int ssPin=11;Spi* spi;};\nSpi spi;ardu_sdcard_t card{11,&spi};ardu_sdcard_t* s_cards[]={&card};\nvoid traceSd(uint32_t,uint32_t,uint32_t,uint32_t=0){}\n'
MAIN = '\nint main(){\n selected=true;spi.replies={0,3,35,255};spi.at=0;nowMs=0;\n assert(sdWait(0,20));assert(spi.at==4); // Partial busy-release bytes are NOT ready.\n spi.replies={0,3,35,0};spi.at=0;nowMs=0;\n assert(!sdWait(0,3));assert(spi.at==3); // Nonzero alone must not bypass timeout.\n spi.replies={0,3,255};spi.at=0;nowMs=UINT32_MAX-1;\n assert(sdWait(0,10));assert(spi.at==3); // Timeout arithmetic survives uptime wrap.\n spi.replies={255,0,3,255};spi.at=0;nowMs=0;selected=false;\n assert(sdSelectCard(0));assert(selected&&spi.at==4); // First byte primes delayed busy.\n spi.replies={255};spi.at=0;releaseClocks=0;\n sdDeselectCard(0);assert(!selected&&releaseClocks==1); // Release clock occurs with CS HIGH.\n}\n'

class SourceGuardTests(unittest.TestCase):
    def test_unknown_core_source_is_rejected(self):
        with self.assertRaises(ValueError):
            sd.patch_source('unreviewed source')

class SdReadyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arduino-cli'):
            raise unittest.SkipTest('Pinned Arduino core not installed')
        data = subprocess.check_output(['arduino-cli','config','get','directories.data'],text=True).strip()
        cls.library = Path(data)/'packages/esp32/hardware/esp32'/sd.EXPECTED_VERSION/'libraries/SD'
        source = cls.library/'src/sd_diskio.cpp'
        if not source.exists():
            raise unittest.SkipTest('Pinned Arduino SD source not installed')
        cls.original = source.read_text()

    def test_actual_ready_select_and_release_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name,source in [('original',self.original),('candidate',sd.patch_source(self.original))]:
                functions = []
                for signature in ['bool sdWait(', 'void sdStop(', 'void sdDeselectCard(', 'bool sdSelectCard(']:
                    a,b = sd.function_span(source,signature)
                    functions.append(source[a:b])
                cpp = tmp/(name+'.cpp')
                cpp.write_text(PREFIX+'\n'.join(functions)+MAIN)
                subprocess.run(['g++','-std=c++17','-funsigned-char','-Wall','-Wextra','-Werror',str(cpp),'-o',str(tmp/name)],check=True)
                result = subprocess.run([str(tmp/name)],capture_output=True)
                self.assertEqual(result.returncode==0,name=='candidate',result.stderr.decode())

    def test_generation_preserves_installed_core_and_build_cache(self):
        original = self.library/'src/sd_diskio.cpp'
        before = (original.read_bytes(),original.stat().st_mtime_ns)
        with tempfile.TemporaryDirectory() as directory:
            generated = sd.prepare(self.library,Path(directory)/'SD')
            patched = generated/'src/sd_diskio.cpp'
            self.assertEqual(patched.read_text(),sd.patch_source(self.original))
            stamp = patched.stat().st_mtime_ns
            sd.prepare(self.library,generated)
            self.assertEqual(patched.stat().st_mtime_ns,stamp)
            self.assertEqual((original.read_bytes(),original.stat().st_mtime_ns),before)
        with self.assertRaises(ValueError):
            sd.prepare(self.library,self.library/'generated')
        with self.assertRaises(ValueError):
            sd.patch_source(self.original+'\n')

if __name__ == '__main__':
    unittest.main()
