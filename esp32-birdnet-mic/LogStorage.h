#pragma once
#include <esp_heap_caps.h>
#include <new>
// A fixed allocation for the life of the firmware. Prefer PSRAM where present;
// boards without it use exactly the same bounded byte budget in internal RAM.
inline void* allocateLogStorage(size_t bytes) {
    void* memory=nullptr;
    if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>=bytes+1024)
        memory=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory)memory=heap_caps_malloc(bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    return memory;
}
template<class T> T* allocateLogObject() {
    void* memory=allocateLogStorage(sizeof(T));
    return memory?new(memory) T():nullptr;
}
