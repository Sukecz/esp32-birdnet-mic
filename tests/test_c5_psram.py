"""XIAO ESP32-C5 PSRAM: larger listening queue, internal fallbacks, and the build helper's PSRAM option."""
import os
import pathlib
import stat
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
BROWSER = (ROOT / 'esp32-birdnet-mic/BrowserAudio.cpp').read_text()


class C5PsramTests(unittest.TestCase):
    def test_listen_queue_external_storage(self):
        code = r'''
#include "PreviewBuffer.h"
#include <cassert>
#include <vector>
int main(){
 static uint8_t big[32768], odd[20000];
 PreviewBuffer q;
 assert(q.queueBytes()==PreviewBuffer::capacity);
 assert(!q.useStorage(nullptr,32768)&&!q.useStorage(odd,20000)&&!q.useStorage(big,PreviewBuffer::capacity));
 PreviewBuffer used;used.begin();assert(!used.useStorage(big,32768));used.end();
 assert(!used.useStorage(big,32768)); // no resizing after the first session
 assert(q.useStorage(big,32768)&&q.queueBytes()==32768);
 assert(!q.useStorage(big,65536)); // no replacement or ownership leak
 q.begin();
 std::vector<uint8_t> in(1024),out(1024);uint8_t next=0,expect=0;
 for(int round=0;round<500;++round){ // many wraps, mixed fill levels
  int n=1+round%30;
  for(int k=0;k<n&&!q.overflowed();++k){for(auto& b:in)b=next++;q.push(in.data(),in.size());}
  if(q.overflowed())break;
  size_t got;while((got=q.pop(out.data(),out.size())))for(size_t i=0;i<got;++i)assert(out[i]==expect++);
 }
 assert(!q.overflowed());
 for(int k=0;k<32;++k)q.push(in.data(),in.size()); // exactly full
 assert(!q.overflowed());q.push(in.data(),1);assert(q.overflowed());
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp, binary = pathlib.Path(temp) / 't.cpp', pathlib.Path(temp) / 't'
            cpp.write_text(code)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_psram_only_on_c5_with_internal_fallbacks(self):
        self.assertIn('#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_SPIRAM\n#define AUDIO_RING_IN_PSRAM 1', SKETCH)
        start = SKETCH[SKETCH.index('bool startAudioProducer() {'):]
        # PSRAM first, then the unchanged internal reserve path as fallback.
        self.assertLess(start.index('createPsramAudioRing(requested)'), start.index('audioreserve::fit(requested'))
        self.assertIn('vRingbufferDeleteWithCaps(audioRingBuffer)', SKETCH)
        # DMA/PDM working buffers stay internal even when malloc() prefers PSRAM above 4 KB.
        self.assertIn('audioWorkAlloc(sizeof(pdmsw::Decimator))', SKETCH)
        self.assertIn('audioWorkAlloc(sizeof(PdmReadBuffers))', SKETCH)
        self.assertNotIn('(PdmReadBuffers*)malloc(', SKETCH)
        for buffer in ('i2s_32bit_buffer', 'i2s_16bit_buffer', 'i2s_16bit_network_buffer', 'rtpPacketScratch'):
            self.assertNotRegex(SKETCH, buffer + r' = \([a-z0-9_]+\*\)malloc\(')
            self.assertRegex(SKETCH, buffer + r' = \([a-z0-9_]+\*\)audioWorkAlloc\(')
        self.assertIn('heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)', SKETCH)
        self.assertIn('#define LOG_MEMORY_TREND 0', SKETCH)
        self.assertIn('#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_SPIRAM', BROWSER)
        self.assertIn('heap_caps_malloc(kPsramQueueBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)', BROWSER)

    def test_compile_helper_enables_c5_psram(self):
        with tempfile.TemporaryDirectory() as temp:
            fake = pathlib.Path(temp) / 'arduino-cli'
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
            sd = pathlib.Path(temp) / 'sd-fixture'
            (sd / 'src').mkdir(parents=True)
            (sd / 'library.properties').write_text('name=SD test fixture\n')
            (sd / 'src/SD.h').write_text('// Argument inspection only; no compilation.\n')
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'], BIRDNETMIC_SD_LIBRARY=str(sd))

            def run(*args):
                out = subprocess.run(['bash', str(ROOT / 'tools/compile_firmware.sh'), *args],
                                     check=True, capture_output=True, text=True, env=env).stdout
                return out.splitlines()

            self.assertIn('esp32:esp32:XIAO_ESP32C5:PSRAM=enabled', run('--fqbn', 'esp32:esp32:XIAO_ESP32C5'))
            self.assertIn('--fqbn=esp32:esp32:XIAO_ESP32C5:PSRAM=enabled', run('--fqbn=esp32:esp32:XIAO_ESP32C5'))
            self.assertIn('esp32:esp32:XIAO_ESP32C5:CPUFreq=160,PSRAM=enabled',
                          run('-b', 'esp32:esp32:XIAO_ESP32C5:CPUFreq=160'))
            self.assertIn('esp32:esp32:XIAO_ESP32C5:PSRAM=disabled', run('--fqbn', 'esp32:esp32:XIAO_ESP32C5:PSRAM=disabled'))
            for other in ('XIAO_ESP32C3', 'XIAO_ESP32S3', 'XIAO_ESP32C6'):
                lines = run('--fqbn', 'esp32:esp32:' + other)
                self.assertIn('esp32:esp32:' + other, lines)
                self.assertFalse(any('PSRAM' in line for line in lines))
            self.assertTrue(any('-flto' in line for line in run('--fqbn', 'esp32:esp32:XIAO_ESP32C6')))


if __name__ == '__main__':
    unittest.main()
