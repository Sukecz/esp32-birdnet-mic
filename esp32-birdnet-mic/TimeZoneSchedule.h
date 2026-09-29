#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

// Persisted values: keep zero as the legacy fixed-offset mode.
enum TimeZoneMode : uint8_t {
    TIME_ZONE_FIXED = 0,
    TIME_ZONE_CENTRAL_EUROPE = 1,
    TIME_ZONE_NEW_ZEALAND = 2,
    TIME_ZONE_UNITED_KINGDOM = 3,
    TIME_ZONE_US_EASTERN = 4,
    TIME_ZONE_US_CENTRAL = 5,
    TIME_ZONE_US_MOUNTAIN = 6,
    TIME_ZONE_US_PACIFIC = 7,
    TIME_ZONE_AUSTRALIA_EASTERN = 8,
    TIME_ZONE_CUSTOM = 9,
};

inline const char* timeZoneRule(uint8_t mode) {
    switch (mode) {
        case TIME_ZONE_CENTRAL_EUROPE: return "CET-1CEST,M3.5.0/2,M10.5.0/3";
        case TIME_ZONE_NEW_ZEALAND: return "NZST-12NZDT,M9.5.0/2,M4.1.0/3";
        case TIME_ZONE_UNITED_KINGDOM: return "GMT0BST,M3.5.0/1,M10.5.0/2";
        case TIME_ZONE_US_EASTERN: return "EST5EDT,M3.2.0/2,M11.1.0/2";
        case TIME_ZONE_US_CENTRAL: return "CST6CDT,M3.2.0/2,M11.1.0/2";
        case TIME_ZONE_US_MOUNTAIN: return "MST7MDT,M3.2.0/2,M11.1.0/2";
        case TIME_ZONE_US_PACIFIC: return "PST8PDT,M3.2.0/2,M11.1.0/2";
        case TIME_ZONE_AUSTRALIA_EASTERN: return "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
        default: return nullptr;
    }
}

inline bool parseTzNumber(const char*& p, int maxValue, int& value) {
    if (*p < '0' || *p > '9') return false;
    value = 0;
    do {
        value = value * 10 + (*p++ - '0');
        if (value > maxValue) return false;
    } while (*p >= '0' && *p <= '9');
    return true;
}

inline bool parseTzClock(const char*& p, int maxHours) {
    if (*p == '+' || *p == '-') ++p;
    int hours, minutes = 0, seconds = 0;
    if (!parseTzNumber(p, maxHours, hours)) return false;
    if (*p == ':') {
        ++p;
        if (!parseTzNumber(p, 59, minutes)) return false;
        if (*p == ':') {
            ++p;
            if (!parseTzNumber(p, 59, seconds)) return false;
        }
    }
    return true;
}

// Deliberately accept only recurring Mmonth.week.weekday rules. Other POSIX
// forms and irregular political changes need a different implementation.
inline bool validCustomTimeZoneRule(const char* rule) {
    if (!rule || strlen(rule) < 16 || strlen(rule) > 80) return false;
    const char* p = rule;
    for (int name = 0; name < 2; ++name) {
        int letters = 0;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            ++p;
            ++letters;
        }
        if (letters < 3) return false;
        if (name == 0 && !parseTzClock(p, 24)) return false;
    }
    if (*p != ',') {
        if (!parseTzClock(p, 24) || *p != ',') return false;
    }
    for (int transition = 0; transition < 2; ++transition) {
        if (*p++ != ',' || *p++ != 'M') return false;
        int month, week, weekday;
        if (!parseTzNumber(p, 12, month) || month < 1 || *p++ != '.' ||
            !parseTzNumber(p, 5, week) || week < 1 || *p++ != '.' ||
            !parseTzNumber(p, 6, weekday)) return false;
        if (*p == '/') {
            ++p;
            if (!parseTzClock(p, 167)) return false;
        }
    }
    return *p == '\0';
}

// Use the UTC instant for the next occurrence of a local start time. Trying both
// DST states also finds the second occurrence of a repeated autumn hour.
inline uint32_t secondsUntilNextLocalMinute(time_t now, uint16_t startMin) {
    struct tm today;
    if (!localtime_r(&now, &today)) return 0;
    time_t next = 0;
    bool found = false;
    for (int day = 0; day <= 2; ++day) {
        for (int dst = -1; dst <= 1; ++dst) {
            struct tm candidate = today;
            candidate.tm_mday += day;
            candidate.tm_hour = startMin / 60;
            candidate.tm_min = startMin % 60;
            candidate.tm_sec = 0;
            candidate.tm_isdst = dst;
            time_t instant = mktime(&candidate);
            if (instant <= now) continue;
            // Reject forced-DST results that map to a different wall-clock
            // minute. For a nonexistent spring minute, mktime's normalized
            // value from the automatic (-1) case is the next usable time.
            if (dst != -1 &&
                (candidate.tm_hour != startMin / 60 || candidate.tm_min != startMin % 60)) continue;
            if (!found || instant < next) {
                next = instant;
                found = true;
            }
        }
        if (found) break;
    }
    if (!found) return 0;
    uint64_t delta = (uint64_t)(next - now);
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}
