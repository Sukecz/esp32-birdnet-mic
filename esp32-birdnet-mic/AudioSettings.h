#pragma once
#include <stdint.h>
inline uint16_t maxHighpassCutoffForRate(uint32_t sampleRate) {
    uint32_t maxCutoff = (uint32_t)((float)sampleRate * 0.45f);
    if (maxCutoff < 10U) maxCutoff = 10U;
    if (maxCutoff > 10000U) maxCutoff = 10000U;
    return (uint16_t)maxCutoff;
}

inline uint16_t normalizeHighpassCutoff(uint32_t sampleRate, uint32_t requestedCutoff) {
    if (requestedCutoff < 10U) return 10U;
    uint16_t maxCutoff = maxHighpassCutoffForRate(sampleRate);
    return requestedCutoff > maxCutoff ? maxCutoff : (uint16_t)requestedCutoff;
}
