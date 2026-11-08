#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace recording {
// Bounded PCM16 mono encoder: Rice-coded order-zero or verbatim fallback.
// RFC 9639. Variable blocking permits short capture/checkpoint chunks.
template<class Sink> class FlacWriter {
    Sink& sink_; uint32_t rate_=0, pcm_=0, stored_=0; bool failed_=false;
    uint8_t frame_[2080]; size_t bits_=0;
    // Pack a byte at a time: per-bit loops cannot keep up with live PDM on
    // an 80 MHz C5. The stream remains bit-for-bit equivalent.
    void bits(uint32_t v,unsigned n) {
        while(n) {
            const unsigned offset=bits_&7, room=8-offset, take=n<room?n:room;
            if(!offset)frame_[bits_/8]=0;
            frame_[bits_/8]|=((v>>(n-take))&((1U<<take)-1))<<(room-take);
            bits_+=take;n-=take;
        }
    }
    static uint16_t crc16(const uint8_t* p,size_t n) {
        static constexpr uint16_t table[16]={0x0000,0x8005,0x800f,0x000a,0x801b,0x001e,0x0014,0x8011,0x8033,0x0036,0x003c,0x8039,0x0028,0x802d,0x8027,0x0022};
        uint16_t crc=0;
        while(n--) {
            const uint8_t byte=*p++;
            crc=uint16_t(crc<<4)^table[((crc>>12)^(byte>>4))&15];
            crc=uint16_t(crc<<4)^table[((crc>>12)^byte)&15];
        }
        return crc;
    }
    static uint32_t fold(int16_t v) { return v<0?uint32_t(-int32_t(v))*2-1:uint32_t(v)*2; }
    static int16_t sample(const uint8_t* p) { return int16_t(uint16_t(p[0])|uint16_t(p[1])<<8); }
    bool put(const uint8_t* p,size_t n) { if(failed_||sink_.write(p,n)!=n){failed_=true;return false;} stored_+=n;return true; }
    void header(uint8_t* h) const {
        memset(h,0,42);memcpy(h,"fLaC",4);h[4]=0x80;h[7]=34;
        h[9]=16;h[10]=4; // minimum 16, maximum 1024 samples
        uint64_t packed=(uint64_t(rate_)<<44)|(uint64_t(15)<<36)|(pcm_/2);
        for(unsigned i=0;i<8;++i)h[18+i]=uint8_t(packed>>(56-8*i));
        // Zero MD5 means unavailable; frames carry mandatory CRCs.
    }
    bool encode(const uint8_t* p,size_t count) {
        unsigned n=count/2;bits_=0;bits(0xfff9,16);bits(0x70,8);bits(0x08,8);
        uint32_t num=pcm_/2;unsigned len=1;
        if(num>=128){len=2;while(num>=(uint32_t(1)<<(5*len+1)))++len;}
        if(len==1)bits(num,8);else {bits(((0xff<<(8-len))&255)|(num>>(6*(len-1))),8);for(unsigned i=len-1;i;--i)bits(0x80|((num>>(6*(i-1)))&63),8);}
        bits(n-1,16);
        uint8_t crc8=0;for(size_t i=0;i<bits_/8;++i){crc8^=frame_[i];for(int j=0;j<8;++j)crc8=uint8_t((crc8<<1)^((crc8&128)?7:0));}bits(crc8,8);
        uint32_t best=uint32_t(n)*16;unsigned rice=0;
        uint32_t sum=0;
        for(unsigned i=0;i<n;++i)sum+=fold(sample(p+i*2));
        for(unsigned k=0;k<15;++k) {
            // sum(floor(v/2^k)) >= floor(sum(v)/2^k) - (n-1).
            // Reject parameters that cannot beat the current best before
            // scanning samples, keeping the exact original Rice choice.
            const uint32_t quotient=sum>>k;
            const uint32_t lower=quotient>n-1?quotient-(n-1):0;
            uint32_t cost=6+uint32_t(n)*(1+k);
            if(cost+lower>=best)continue;
            for(unsigned i=0;i<n && cost<best;++i)cost+=fold(sample(p+i*2))>>k;
            if(cost<best){best=cost;rice=k;}
        }
        if(best==uint32_t(n)*16){bits(2,8);for(unsigned i=0;i<n;++i)bits(uint16_t(sample(p+i*2)),16);}
        else {bits(0x10,8);bits(0,6);bits(rice,4);for(unsigned i=0;i<n;++i){uint32_t v=fold(sample(p+i*2));for(uint32_t q=v>>rice;q;--q)bits(0,1);bits(1,1);bits(v,rice);}}
        while(bits_&7)bits(0,1);
        const uint16_t crc=crc16(frame_,bits_/8);bits(crc,16);
        if(!put(frame_,bits_/8))return false;
        pcm_+=count;return true;
    }
public:
    explicit FlacWriter(Sink& s):sink_(s){}
    bool begin(uint32_t rate){rate_=rate;pcm_=stored_=0;failed_=false;uint8_t h[42];header(h);return put(h,42);}
    bool append(const uint8_t* p,size_t n){if(n%2||failed_)return false;while(n){size_t chunk=n>2048?2048:n;if(!encode(p,chunk))return false;p+=chunk;n-=chunk;}return true;}
    bool checkpoint(){uint8_t h[42];header(h);uint32_t end=stored_;if(failed_||!sink_.seek(0)||sink_.write(h,42)!=42||!sink_.sync()||!sink_.seek(end)){failed_=true;return false;}return true;}
    uint32_t bytes()const{return pcm_;}
    uint32_t storedBytes()const{return stored_;}
    void expectedHeader(uint8_t* h)const{header(h);}
};
}
