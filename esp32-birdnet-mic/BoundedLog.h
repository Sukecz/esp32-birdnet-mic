#pragma once
#include <stddef.h>
#include <string.h>
// Complete newline-delimited messages, bounded in bytes rather than growing Strings.
// Caller owns synchronization. No allocation, even after arbitrary wrap-around.
template<size_t Capacity> class BoundedLog {
    static_assert(Capacity > 2, "Log capacity");
    char bytes[Capacity] = {};
    size_t head = 0, used = 0;
public:
    size_t size() const { return used; }
    constexpr size_t capacity() const { return Capacity; }
    void clear() { head = used = 0; }
    char at(size_t index) const { return bytes[(head+index)%Capacity]; }
    void dropLine() {
        while (used) { char c=bytes[head]; head=(head+1)%Capacity; --used; if(c=='\n')break; }
    }
    size_t append(const char* text, size_t length) {
        if (!text) return false;
        while(length && (text[length-1]=='\n' || text[length-1]=='\r'))--length;
        if(length>Capacity-1)length=Capacity-1;
        size_t discarded=0;
        while(used+length+1>Capacity){dropLine();++discarded;}
        for(size_t i=0;i<length;++i){bytes[(head+used)%Capacity]=text[i];++used;}
        bytes[(head+used)%Capacity]='\n';++used;
        return discarded;
    }
    bool pop(char* out,size_t capacity) {
        if(!used || !capacity)return false;
        size_t n=0;
        while(used){char c=bytes[head];head=(head+1)%Capacity;--used;if(c=='\n')break;if(n+1<capacity)out[n++]=c;}
        out[n]=0;return true;
    }
};
