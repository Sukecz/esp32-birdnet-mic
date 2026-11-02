#include "NoiseShadow.h"
#include <atomic>
#include <algorithm>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "AudioReserve.h"
#define NSIG_HOT IRAM_ATTR __attribute__((noinline))
#include "NoiseSignature.h"

namespace noiseshadow {
namespace {
constexpr uint32_t pack(uint8_t mode,uint8_t floor,uint8_t adapt) { return mode|(uint32_t(floor)<<8)|(uint32_t(adapt)<<16); }
constexpr uint32_t defaults=pack(0,12,15);
bool valid(uint8_t m,uint8_t f,uint8_t a) { return m<=1 && (f==6 || f==12 || f==20) && (a==4 || a==15 || a==60); }
std::atomic<uint32_t> requested{defaults};
nsig::Engine* engine=nullptr;
uint32_t applied=0, lastPublish=0;
Chain chain{};
bool psram=false, stoppedForLoad=false;
uint16_t lateBlocks=0;
uint64_t busyUs=0,audioUs=0;
float cpu=0,maximum=0,reduction=0;
char error[64]="";
struct Snapshot {
    bool active=false,learned=false,psram=false;
    uint32_t rate=48000,learn=0;
    float cpu=0,peak=0,reduction=0;
    char error[64]="";
    uint16_t level[nsig::kBins]{},floor[nsig::kBins]{},signature[nsig::kBins]{};
};
Snapshot published,webSnapshot; // no engine or spectrum on either task's stack
SemaphoreHandle_t snapshotLock=nullptr;
void publish(bool force=false) {
    const uint32_t now=millis();
    if(!force && now-lastPublish<500) return;
    // A busy web reader only skips telemetry, never an audio block.
    if(!snapshotLock || xSemaphoreTake(snapshotLock,0)!=pdTRUE) return;
    lastPublish=now;
    published.active=engine!=nullptr && !stoppedForLoad;
    published.psram=psram; published.cpu=cpu; published.peak=maximum; published.reduction=reduction;
    strncpy(published.error,error,sizeof(published.error));
    published.learned=false; published.learn=0;
    if(engine) {
        published.rate=engine->sampleRate;
        published.learned=nsig::signatureLearned(engine);
        published.learn=engine->learnFrames?uint32_t(std::min(uint64_t(100),uint64_t(engine->sigFrames)*100/engine->learnFrames)):0;
        memcpy(published.level,engine->smooth,sizeof(published.level));
        memcpy(published.floor,engine->floorQ8,sizeof(published.floor));
        memcpy(published.signature,engine->sig,sizeof(published.signature));
    }
    xSemaphoreGive(snapshotLock);
}
void freeEngine() {
    if(engine) { heap_caps_free(engine); engine=nullptr; }
    psram=false; busyUs=audioUs=0; cpu=maximum=reduction=0; lateBlocks=0;
}
bool allocate(const Chain& c,uint32_t config) {
    const size_t need=sizeof(nsig::Engine);
#if CONFIG_SPIRAM
    engine=static_cast<nsig::Engine*>(heap_caps_malloc(need,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    psram=engine!=nullptr;
#endif
    if(!engine) {
        const auto caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
        // Reuse the audio reserve; do not invent a lower Wi-Fi allowance.
        if(heap_caps_get_free_size(caps)<need+audioreserve::kHeapMargin || heap_caps_get_largest_free_block(caps)<need) {
            strcpy(error,"Insufficient memory; audio is unfiltered."); return false;
        }
        engine=static_cast<nsig::Engine*>(heap_caps_malloc(need,caps));
    }
    if(!engine) { strcpy(error,"Allocation failed; audio is unfiltered."); return false; }
    nsig::Params params; params.gainFloorDb=-float((config>>8)&255); params.subwindowSec=float((config>>16)&255); params.learnSec=120;
    nsig::init(engine,c.sampleRate,params); chain=c; error[0]=0;
    return true;
}
bool same(const Chain& a,const Chain& b) {
    return a.sampleRate==b.sampleRate && a.micFormat==b.micFormat && a.shift==b.shift && a.highpass==b.highpass && a.highpassHz==b.highpassHz && a.gainQ==b.gainQ;
}
}
void begin() {
    if(!snapshotLock) snapshotLock=xSemaphoreCreateMutex();
    Preferences p;
    if(p.begin("noisesig",true)) {
        const uint32_t value=p.getUInt("config",defaults); p.end();
        if(valid(value&255,(value>>8)&255,(value>>16)&255)) requested.store(value);
    }
}
bool save(uint8_t mode,uint8_t floor,uint8_t adapt) {
    if(!valid(mode,floor,adapt)) return false;
    const uint32_t value=pack(mode,floor,adapt);
    if(requested.load()==value) return true;
    Preferences p; if(!p.begin("noisesig",false)) return false;
    const bool ok=p.putUInt("config",value)==sizeof(value); p.end();
    if(ok) requested.store(value); // a failed write preserves runtime and draft
    return ok;
}
void resetSettings() { save(0,12,15); }
void releaseForRestart() {
    // The producer has stopped. A forced task deletion may orphan its brief
    // telemetry lock; HTTP runs synchronously on this caller's task.
    if(snapshotLock) {
        if(xSemaphoreTake(snapshotLock,0)==pdTRUE) xSemaphoreGive(snapshotLock);
        else { vSemaphoreDelete(snapshotLock); snapshotLock=xSemaphoreCreateMutex(); }
    }
    freeEngine(); applied=0; stoppedForLoad=false; error[0]=0; publish(true);
}
void process(int16_t* pcm,int n,const Chain& c) {
    const uint32_t config=requested.load();
    if(config!=applied) { freeEngine(); applied=config; stoppedForLoad=false; error[0]=0; }
    if(!(config&255)) { publish(); return; }
    if(n<=0 || !c.sampleRate || stoppedForLoad) { publish(); return; }
    if(!engine && error[0]) { publish(); return; } // explicit off/on or restart retries
    if(!engine && !allocate(c,config)) { publish(true); return; }
    if(!same(chain,c)) { chain=c; nsig::relearn(engine,96); }
    const auto start=esp_timer_get_time();
    nsig::processInPlace(engine,pcm,n,true);
    const uint64_t elapsed=esp_timer_get_time()-start;
    const uint64_t budget=uint64_t(n)*1000000/c.sampleRate;
    busyUs+=elapsed; audioUs+=budget;
    maximum=std::max(maximum,100.0f*elapsed/std::max(uint64_t(1),budget));
    if(elapsed>=budget) ++lateBlocks; else if(lateBlocks) --lateBlocks;
    if(lateBlocks>=8) {
        freeEngine(); stoppedForLoad=true;
        strcpy(error,"CPU too slow; audio is unfiltered. Raise CPU frequency.");
    } else if(audioUs>=2000000) {
        cpu=100.0f*busyUs/audioUs; busyUs=audioUs=0;
        const auto in=engine->stats.inEnergy,out=engine->stats.outEnergy;
        reduction=in&&out?(int32_t(nsig::log2Q8(in))-int32_t(nsig::log2Q8(out)))*(3.0103f/256):0;
        engine->stats.inEnergy=engine->stats.outEnergy=0;
    }
    publish();
}
String statusJson(bool spectra) {
    const uint32_t config=requested.load();
    if(snapshotLock && xSemaphoreTake(snapshotLock,pdMS_TO_TICKS(20))==pdTRUE) {
        webSnapshot=published; xSemaphoreGive(snapshotLock);
    }
    const auto& s=webSnapshot;
    String json; json.reserve(spectra?6500:480);
    json="{\"mode\":"+String(config&255)+",\"floor_db\":"+String((config>>8)&255)+",\"adapt_s\":"+String((config>>16)&255);
    json+=",\"active\":"+String(s.active?"true":"false")+",\"learned\":"+String(s.learned?"true":"false")+",\"learn_pct\":"+String(s.learn);
    json+=",\"cpu_pct\":"+String(s.cpu,1)+",\"cpu_peak_pct\":"+String(s.peak,1)+",\"est_reduction_db\":"+String(s.reduction,1);
    json+=",\"engine_psram\":"+String(s.psram?"true":"false")+",\"sample_rate\":"+String(s.rate)+",\"error\":\""+s.error+"\"";
    if(spectra && s.active) {
        const uint16_t* arrays[]={s.level,s.floor,s.signature}; const char* names[]={"level","floor","signature"};
        for(int a=0;a<3;++a) { json+=",\""; json+=names[a]; json+="\":["; for(int k=0;k<nsig::kBins;++k) { if(k) json+=','; json+=String(arrays[a][k]); } json+=']'; }
    }
    return json+'}';
}
}
