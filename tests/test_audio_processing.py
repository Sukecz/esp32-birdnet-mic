from firmware_source import firmware_source
"""Numerical DSP reference and memory/rate regressions for the PDM integration."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AudioProcessingTests(unittest.TestCase):
    def native(self, source):
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / 'test.cpp'
            binary = pathlib.Path(temp) / 'test'
            cpp.write_text(source)
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_integer_audio_against_reference(self):
        self.native((ROOT / 'tests/fixed_audio_test.cpp').read_text())

    def test_reserve_retains_wifi_and_task_headroom(self):
        self.native(r'''
#include "AudioReserve.h"
#include <cassert>
int main() {
 using namespace audioreserve;
 assert(fit(65536,1024,215*1024,150*1024)==65536); // C6/S3 keep full reserve
 auto capacity=fit(65536,1024,120*1024,100*1024); // constrained C3/C5
 assert(capacity>=16384 && capacity<65536);
 assert(120*1024-capacity>=kHeapMargin+kProducerStackBytes+kAllocationOverhead);
 assert(fit(65536,1024,215*1024,33000)==28672); // fragmented heap
 assert(fit(65536,1024,90*1024,80*1024)==0); // cannot keep the minimum safely
 assert(fit(65536,1024,215*1024,16384)==0);
 assert(fit(65536,16384,150*1024,150*1024)==0); // four large chunks cannot fit
 assert(fit(65536,1024,0,0)==0);
}
''')

    def test_actual_c5_reserve_selection_boot_reconfigure_and_exhaustion(self):
        source = firmware_source(ROOT)
        start = source.index('    const size_t requested =', source.index('bool startAudioProducer() {'))
        end = source.index('    audioRingBuffer = createInternalAudioRing', start)
        for c5 in (False, True):
            with self.subTest(c5=c5):
                self.native(('#define CONFIG_IDF_TARGET_ESP32C5 1\n' if c5 else '') + r'''
#include "AudioReserve.h"
#include <cassert>
#include <cstdint>
#include <string>
constexpr uint32_t MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
size_t heapFreeTest=0, heapLargestTest=0, audioRingBufferCapacityBytes=0;
int currentBufferSize=512;
size_t computeAudioRingBufferCapacity(){return 65536;}
size_t heap_caps_get_free_size(uint32_t){return heapFreeTest;}
size_t heap_caps_get_largest_free_block(uint32_t){return heapLargestTest;}
std::string String(uint32_t n){return std::to_string(n);}
void simplePrintln(const std::string&){}
size_t choose(){
''' + source[start:end] + r'''
 return audioRingBufferCapacityBytes;
}
int main(){
 heapFreeTest=215*1024; heapLargestTest=150*1024;
 assert(choose()==65536);
 heapFreeTest=104*1024; heapLargestTest=80*1024;
 assert(choose()==16384); // C5 boot after freeing 12 KiB of static RAM.
 heapFreeTest=101*1024;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
 assert(choose()==16384); // Original boot-loop report, I2S and PDM share this path.
 heapFreeTest=90*1024;
 assert(choose()==16384); // Reconfiguration with an existing network client.
 const size_t required=16384+audioreserve::kC5MinimumHeapMargin+
     audioreserve::kProducerStackBytes+audioreserve::kAllocationOverhead;
 heapFreeTest=required;
 assert(choose()==16384);
 heapFreeTest=required-1;
 assert(choose()==0); // A contiguous block alone cannot authorize capture.
 heapFreeTest=101*1024; heapLargestTest=16384+audioreserve::kAllocationOverhead-1;
 assert(choose()==0); // Fragmentation must still reject the allocation.
 heapLargestTest=80*1024; currentBufferSize=8192;
 assert(choose()==0); // Never shrink below four complete capture chunks.
#else
 assert(choose()==0); // C3/S3/C6 retain the existing 80 KiB policy.
#endif
 heapFreeTest=0; heapLargestTest=0;
 assert(choose()==0);
}
''')

    def test_word_rate_startup_overruns_retry_and_reset(self):
        self.native(r'''
#include "PdmCapture.h"
#include <cassert>
int main() {
 for(int stride:{1,2}) {
  pdmsw::WordRateDetector rate;
  assert(rate.observe(999999,0,192000,0)==0); // discard startup burst
  assert(rate.observe(999999,100000,192000,0)==0); // begin steady window
  for(int n=1;n<=4;++n)
   assert(rate.observe(19200*stride,100000+n*100000,192000,0)==(n==4?stride:0));
  assert(rate.observe(0,2000000,192000,9)==stride); // confirmed state stays stable
  rate.reset(); assert(!rate.started && rate.stride==0);
 }
 pdmsw::WordRateDetector rate;
 rate.observe(0,0,192000,0);rate.observe(0,100000,192000,0);
 assert(rate.observe(76800,500000,192000,1)==0); // lost pairs must not become plain
 rate.observe(0,600000,192000,1);
 assert(rate.observe(153600,1000000,192000,1)==2);
 rate.reset();rate.observe(0,0,192000,0);rate.observe(0,100000,192000,0);
 assert(rate.observe(115200,500000,192000,0)==0); // ambiguous rate, retry
 assert(rate.observe(153600,900000,192000,0)==2);
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'))


if __name__ == '__main__':
    unittest.main()
