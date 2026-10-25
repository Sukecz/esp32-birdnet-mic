#include "../esp32-birdnet-mic/RecordingWav.h"
#include <cassert>
#include <vector>
#include <algorithm>
struct Sink {
    std::vector<uint8_t> bytes;
    size_t position = 0;
    bool shortWrite = false, failSync = false, failSeek = false;
    size_t write(const uint8_t* p, size_t n) {
        if (shortWrite && n) --n;
        if (bytes.size() < position+n) bytes.resize(position+n);
        std::copy(p,p+n,bytes.begin()+position); position += n; return n;
    }
    bool seek(size_t n) { if(failSeek)return false;position=n;return true; }
    bool sync() {return !failSync;}
};
uint32_t le32(const std::vector<uint8_t>& b,size_t at){return b[at]|uint32_t(b[at+1])<<8|uint32_t(b[at+2])<<16|uint32_t(b[at+3])<<24;}
int main(){
    Sink s;recording::WavWriter<Sink> w(s);
    assert(!w.begin(0));assert(w.begin(48000));
    const uint8_t pcm[]={0x00,0x80,0xff,0x7f,0,0,0xff,0xff};
    assert(w.append(pcm,8));assert(w.checkpoint());
    assert(s.bytes.size()==52&&s.position==52&&le32(s.bytes,4)==44&&le32(s.bytes,24)==48000&&le32(s.bytes,40)==8);
    assert(std::equal(pcm,pcm+8,s.bytes.begin()+44));
    assert(w.append(pcm,8));assert(w.checkpoint());assert(le32(s.bytes,40)==16&&s.position==60);
    assert(!w.append(pcm,3));
    s.shortWrite=true;assert(!w.append(pcm,8));s.shortWrite=false;
    assert(!w.checkpoint()&&!w.append(pcm,8)); // A short write permanently poisons this segment.
    for(int failure=0;failure<2;++failure){
        Sink failed;recording::WavWriter<Sink> f(failed);assert(f.begin(8000));assert(f.append(pcm,8));
        failed.failSync=failure==0;failed.failSeek=failure==1;assert(!f.checkpoint());
        failed.failSeek=failed.failSync=false;assert(!f.checkpoint());
    }
    Sink header;header.shortWrite=true;recording::WavWriter<Sink> h(header);assert(!h.begin(48000));assert(!h.checkpoint());
}
