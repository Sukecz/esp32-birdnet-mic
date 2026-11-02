#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

// Pure request validation; independent of Arduino, HTTP and persistence.
namespace settingsvalidation {
template<class String>
static bool parseUInt32Strict(const String& input, uint32_t& out) {
    const char* s = input.c_str();
    size_t i = 0;
    if (s[i] == '\0') return false;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' || v > UINT32_MAX) return false;
    out = (uint32_t)v;
    return true;
}

template<class String>
static bool parseInt32Strict(const String& input, int32_t& out) {
    const char* s = input.c_str();
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    if (s[i] == '\0') return false;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (v < (long)INT32_MIN || v > (long)INT32_MAX) return false;
    out = (int32_t)v;
    return true;
}

template<class String>
static bool parseFloatStrict(const String& input, float& out) {
    const char* s = input.c_str();
    if (*s == '\0') return false;
    errno = 0;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (!isfinite(v)) return false;
    out = v;
    return true;
}


inline bool validSchedule(bool enabled, bool sleep, uint32_t start, uint32_t stop) {
    return start < 1440 && stop < 1440 && (!enabled || start != stop) && (!sleep || enabled);
}
}
