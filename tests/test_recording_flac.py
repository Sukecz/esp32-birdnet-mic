"""Independent decoder regression for the bounded on-device encoder."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg independent decoder required')
class FlacTest(unittest.TestCase):
    def test_pcm_roundtrip_and_checkpoints(self):
        source = r'''
#include <cstdio>
#include <cassert>
#include <vector>
#include "RecordingFlac.h"
struct Sink {
 FILE* f; bool fail=false;
 size_t write(const uint8_t* p,size_t n){return fail?0:fwrite(p,1,n,f);}
 bool seek(uint32_t n){return !fseek(f,n,SEEK_SET);} bool sync(){return !fflush(f);}
};
int main(int argc,char** argv){
 assert(argc==2);unsigned rate=unsigned(atoi(argv[1]));
 Sink s;s.f=fopen("audio.flac","wb");recording::FlacWriter<Sink> f(s);
 assert(f.begin(rate));FILE* raw=fopen("audio.raw","wb");
 uint32_t rng=1;std::vector<uint8_t> p(4096);
 for(unsigned j=0;j<120;++j){
  size_t n=j==119?34:p.size();
  for(size_t i=0;i<n;i+=2){rng=rng*1664525+1013904223;int16_t v=j<20?0:j<40?32767:j<60?-32768:j<80?int16_t(rng%16):int16_t(rng>>16);p[i]=uint8_t(v);p[i+1]=uint16_t(v)>>8;}
  assert(f.append(p.data(),n));assert(f.checkpoint());fwrite(p.data(),1,n,raw);
 }
 assert(!f.append(p.data(),1));s.fail=true;assert(!f.append(p.data(),32));assert(!f.checkpoint());
 fclose(raw);fclose(s.f);
}
'''
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d/'test.cpp').write_text('#include <cstdlib>\n'+source)
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'esp32-birdnet-mic'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
            # Keep an independent, simple per-bit reference to ensure the faster
            # on-device packer and CRC produce the exact existing FLAC stream.
            header = (ROOT/'esp32-birdnet-mic/RecordingFlac.h').read_text()
            begin = header.index('    void bits(')
            end = header.index('    static uint32_t fold', begin)
            reference = header[:begin] + r"""
    void bits(uint32_t v,unsigned n) {
        for(unsigned i=n;i;--i) {
            size_t at=bits_++;
            if(!(at&7))frame_[at/8]=0;
            frame_[at/8]|=((v>>(i-1))&1)<<(7-(at&7));
        }
    }
    static uint16_t crc16(const uint8_t* p,size_t n) {
        uint16_t crc=0;
        for(size_t i=0;i<n;++i) {
            crc^=uint16_t(p[i])<<8;
            for(int j=0;j<8;++j)crc=uint16_t((crc<<1)^((crc&0x8000)?0x8005:0));
        }
        return crc;
    }
""" + header[end:]
            rice_begin = reference.index('        uint32_t sum=0;')
            rice_end = reference.index('        if(best==uint32_t(n)*16)', rice_begin)
            reference = reference[:rice_begin] + r'''
        for(unsigned k=0;k<15;++k){uint32_t cost=6;for(unsigned i=0;i<n;++i)cost+=(fold(sample(p+i*2))>>k)+1+k;if(cost<best){best=cost;rice=k;}}
''' + reference[rice_end:]
            ref = d/'reference'; ref.mkdir()
            (ref/'RecordingFlac.h').write_text(reference)
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',
                            '-I'+str(ref),str(d/'test.cpp'),'-o',str(ref/'test')],check=True)
            for rate in (8000,44100,48000,96000):
                subprocess.run([str(d/'test'),str(rate)],cwd=d,check=True)
                subprocess.run(['ffmpeg','-v','error','-xerror','-i','audio.flac','-f','s16le','-y','decoded.raw'],cwd=d,check=True)
                self.assertEqual((d/'audio.raw').read_bytes(),(d/'decoded.raw').read_bytes())
                self.assertLess((d/'audio.flac').stat().st_size,(d/'audio.raw').stat().st_size)
                subprocess.run([str(ref/'test'),str(rate)],cwd=ref,check=True)
                self.assertEqual((d/'audio.flac').read_bytes(),(ref/'audio.flac').read_bytes())
