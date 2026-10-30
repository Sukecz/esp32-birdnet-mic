#pragma once
#if defined(__has_include)
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#endif

// Both PSRAM-equipped XIAO targets use the same CPU-accessible audio storage.
// DMA and capture/DSP working buffers always remain in internal RAM.
#if (defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32S3)) && CONFIG_SPIRAM
#define AUDIO_RING_IN_PSRAM 1
#else
#define AUDIO_RING_IN_PSRAM 0
#endif
