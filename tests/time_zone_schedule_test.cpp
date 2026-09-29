#include "../esp32-birdnet-mic/TimeZoneSchedule.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static time_t utc(int year, int month, int day, int hour, int minute) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    return timegm(&t);
}

static void expectLocal(time_t instant, int hour, int minute, int dst) {
    struct tm local;
    assert(localtime_r(&instant, &local));
    assert(local.tm_hour == hour && local.tm_min == minute);
    assert((local.tm_isdst > 0) == (dst > 0));
}

int main() {
    assert(timeZoneRule(TIME_ZONE_FIXED) == nullptr);
    assert(timeZoneRule(TIME_ZONE_CENTRAL_EUROPE));
    assert(timeZoneRule(TIME_ZONE_NEW_ZEALAND));
    for (int mode = TIME_ZONE_CENTRAL_EUROPE; mode <= TIME_ZONE_AUSTRALIA_EASTERN; ++mode)
        assert(validCustomTimeZoneRule(timeZoneRule(mode)));
    assert(validCustomTimeZoneRule("IST-5:30IDT-6:30,M3.5.0/2,M10.5.0/2"));
    assert(!validCustomTimeZoneRule("EST5EDT,M3.2.0/2,M13.1.0/2"));
    assert(!validCustomTimeZoneRule("EST5EDT,M3.2.0/2"));
    assert(!validCustomTimeZoneRule("EST5EDT,M3.2.0/2,M11.1.0/2;bad"));

    setenv("TZ", "UTC-5:45", 1);
    tzset();
    expectLocal(utc(2026, 1, 1, 0, 0), 5, 45, 0);
    setenv("TZ", "UTC+3:30", 1);
    tzset();
    expectLocal(utc(2026, 1, 1, 0, 0), 20, 30, 0);

    setenv("TZ", timeZoneRule(TIME_ZONE_CENTRAL_EUROPE), 1);
    tzset();
    expectLocal(utc(2026, 3, 29, 0, 59), 1, 59, 0);
    expectLocal(utc(2026, 3, 29, 1, 0), 3, 0, 1);
    expectLocal(utc(2026, 10, 25, 0, 59), 2, 59, 1);
    expectLocal(utc(2026, 10, 25, 1, 0), 2, 0, 0);
    // A 03:30 schedule must wake at 03:30 local, even across the jump.
    assert(secondsUntilNextLocalMinute(utc(2026, 3, 28, 22, 0), 3 * 60 + 30)
           == 3 * 3600 + 30 * 60);
    // A nonexistent 02:30 start is normalized to the first valid 03:30.
    assert(secondsUntilNextLocalMinute(utc(2026, 3, 29, 0, 30), 2 * 60 + 30)
           == 60 * 60);
    // A repeated 02:30 exists twice; after the first one, find the second.
    assert(secondsUntilNextLocalMinute(utc(2026, 10, 25, 0, 40), 2 * 60 + 30)
           == 50 * 60);

    setenv("TZ", timeZoneRule(TIME_ZONE_NEW_ZEALAND), 1);
    tzset();
    expectLocal(utc(2026, 9, 26, 13, 59), 1, 59, 0);
    expectLocal(utc(2026, 9, 26, 14, 0), 3, 0, 1);
    expectLocal(utc(2027, 4, 3, 13, 59), 2, 59, 1);
    expectLocal(utc(2027, 4, 3, 14, 0), 2, 0, 0);
    assert(secondsUntilNextLocalMinute(utc(2026, 9, 26, 11, 0), 3 * 60 + 30)
           == 3 * 3600 + 30 * 60);

    setenv("TZ", timeZoneRule(TIME_ZONE_US_EASTERN), 1);
    tzset();
    expectLocal(utc(2026, 3, 8, 6, 59), 1, 59, 0);
    expectLocal(utc(2026, 3, 8, 7, 0), 3, 0, 1);
    setenv("TZ", timeZoneRule(TIME_ZONE_AUSTRALIA_EASTERN), 1);
    tzset();
    expectLocal(utc(2026, 10, 3, 15, 59), 1, 59, 0);
    expectLocal(utc(2026, 10, 3, 16, 0), 3, 0, 1);
    puts("time zone and DST schedule tests passed");
}
