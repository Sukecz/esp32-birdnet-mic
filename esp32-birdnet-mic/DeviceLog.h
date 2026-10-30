#pragma once
#include <esp_heap_caps.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Main-loop only. Fixed byte budgets, with absolute cursors for bounded HTTP pages.
class DeviceLog {
    char* data=nullptr;
    size_t capacityBytes=0, head=0, used=0;
    uint64_t first=0, last=0;
    bool external=false, selectedMode=false, configured=false;
    void dropLine() {
        while(used) {const char c=data[head];head=(head+1)%capacityBytes;--used;++first;if(c=='\n')break;}
    }
    bool resizeInternal(size_t target) {
        if(capacityBytes==target)return true;
        char* replacement=static_cast<char*>(heap_caps_malloc(target,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        if(!replacement)return false;
        size_t skip=0,keep=used;
        while(keep>target) {while(keep){char c=at(skip++);--keep;if(c=='\n')break;}}
        for(size_t i=0;i<keep;++i)replacement[i]=at(skip+i);
        if(data)heap_caps_free(data);
        data=replacement;capacityBytes=target;head=0;used=keep;first+=skip;
        return true;
    }
public:
    static constexpr size_t kExternalBytes=32768, kStreamingBytes=8192, kRecordingBytes=4096, kPageBytes=2048;
    struct Page { uint64_t first=0,last=0,start=0,end=0;size_t bytes=0;bool cursorMoved=false; };
    ~DeviceLog(){if(data)heap_caps_free(data);}
    DeviceLog()=default;
    DeviceLog(const DeviceLog&)=delete;
    DeviceLog& operator=(const DeviceLog&)=delete;
    bool configure(bool recording) {
        if(configured && selectedMode==recording)return data!=nullptr;
        selectedMode=recording;configured=true;
        if(external)return true;
        if(!data && heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>=kExternalBytes+1024) {
            data=static_cast<char*>(heap_caps_malloc(kExternalBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
            if(data){capacityBytes=kExternalBytes;external=true;return true;}
        }
        const bool ok=resizeInternal(recording?kRecordingBytes:kStreamingBytes);
        // Failed growth retains the previous, smaller buffer. Failed shrink can
        // be retried on the next transition; no unbounded allocation is made.
        return ok;
    }
    size_t size()const{return used;}
    size_t capacity()const{return capacityBytes;}
    bool usesPsram()const{return external;}
    char at(size_t index)const{return data[(head+index)%capacityBytes];}
    uint64_t beginCursor()const{return first;}
    uint64_t endCursor()const{return last;}
    void append(const char* text,size_t length) {
        if(!data || !text)return;
        while(length && (text[length-1]=='\n'||text[length-1]=='\r'))--length;
        if(length>1023)length=1023; // Every message fits within a single HTTP page.
        if(length>=capacityBytes)length=capacityBytes-1;
        while(used+length+1>capacityBytes)dropLine();
        for(size_t i=0;i<length;++i){data[(head+used)%capacityBytes]=text[i];++used;++last;}
        data[(head+used)%capacityBytes]='\n';++used;++last;
    }
    size_t copy(size_t offset,char* output,size_t maximum)const {
        if(offset>=used)return 0;
        size_t n=used-offset;if(n>maximum)n=maximum;
        for(size_t i=0;i<n;++i)output[i]=at(offset+i);
        return n;
    }
    Page page(char* output,size_t maximum,bool after=false,bool haveCursor=false,uint64_t cursor=0)const {
        Page p;p.first=first;p.last=last;
        if(!used || !maximum){p.start=p.end=first;return p;}
        if(maximum>kPageBytes)maximum=kPageBytes;
        uint64_t boundary=haveCursor?cursor:last;
        if(boundary<first){boundary=first;p.cursorMoved=true;}
        if(boundary>last){boundary=last;p.cursorMoved=true;}
        size_t start=0,end=used;
        if(after && haveCursor) {
            start=size_t(boundary-first);
            while(start<used && start && at(start-1)!='\n')++start;
            end=start+maximum;if(end>used)end=used;
            while(end>start && at(end-1)!='\n')--end;
        }else {
            end=size_t(boundary-first);
            while(end && at(end-1)!='\n')--end;
            start=end>maximum?end-maximum:0;
            while(start<end && start && at(start-1)!='\n')++start;
        }
        p.start=first+start;p.end=first+end;p.bytes=end-start;
        copy(start,output,p.bytes);return p;
    }
    static bool parseCursor(const char* text,uint64_t& value) {
        if(!text || !*text)return false;
        value=0;
        for(;*text;++text){if(*text<'0'||*text>'9')return false;const uint64_t digit=*text-'0';if(value>(UINT64_MAX-digit)/10)return false;value=value*10+digit;}
        return true;
    }
};
