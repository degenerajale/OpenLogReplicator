/* Checks Ctx::toEpoch: fixed-offset mode is unchanged, and a named host zone resolves the
 * UTC offset per wall-clock hour across DST transitions, including the hour-bucket cache.
 * Also times both modes so a regression in the hot path shows up.
 *
 * Build: part of -DWITH_TESTS=ON (target test_timezone). Needs /usr/share/zoneinfo. */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "../src/common/Ctx.h"
#include "../src/common/types/Time.h"

using namespace OpenLogReplicator;

namespace {
    // Redo wall-clock encoding: sec + 60*(min + 60*(hour + 24*((day-1) + 31*((mon-1) + 12*(year-1988)))))
    Time wall(int year, int mon, int day, int hour, int min, int sec) {
        uint64_t v = year - 1988;
        v = v * 12 + (mon - 1);
        v = v * 31 + (day - 1);
        v = v * 24 + hour;
        v = v * 60 + min;
        v = v * 60 + sec;
        return Time(static_cast<uint32_t>(v));
    }

    time_t utc(int year, int mon, int day, int hour, int min, int sec) {
        struct tm tm{};
        tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
        tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
        return timegm(&tm);
    }

    int failures = 0;
    void check(const char* name, time_t got, time_t want) {
        if (got == want)
            std::printf("PASS %s\n", name);
        else {
            std::printf("FAIL %s: got %ld want %ld (diff %ld s)\n", name, static_cast<long>(got), static_cast<long>(want), static_cast<long>(got - want));
            ++failures;
        }
    }
}

int main() {
    static Ctx ctx;

    // 1. Fixed offset mode (hostTimezoneName empty): unchanged behaviour.
    ctx.hostTimezone = -4 * 3600;
    check("fixed -04:00", ctx.toEpoch(wall(2026, 7, 1, 12, 0, 0)), utc(2026, 7, 1, 16, 0, 0));

    // 2. Named zone: New York, summer (EDT, -4) and winter (EST, -5).
    setenv("TZ", "America/New_York", 1);
    tzset();
    ctx.hostTimezoneName = "America/New_York";
    check("NY summer", ctx.toEpoch(wall(2026, 7, 1, 12, 0, 0)), utc(2026, 7, 1, 16, 0, 0));
    check("NY winter", ctx.toEpoch(wall(2026, 1, 15, 12, 0, 0)), utc(2026, 1, 15, 17, 0, 0));

    // 3. Around the fall-back transition on 2026-11-01 02:00 local (EDT -> EST).
    check("NY 2026-11-01 00:30 (EDT)", ctx.toEpoch(wall(2026, 11, 1, 0, 30, 0)), utc(2026, 11, 1, 4, 30, 0));
    check("NY 2026-11-01 03:30 (EST)", ctx.toEpoch(wall(2026, 11, 1, 3, 30, 0)), utc(2026, 11, 1, 8, 30, 0));
    // Around spring-forward on 2027-03-14 02:00 local (EST -> EDT).
    check("NY 2027-03-14 01:30 (EST)", ctx.toEpoch(wall(2027, 3, 14, 1, 30, 0)), utc(2027, 3, 14, 6, 30, 0));
    check("NY 2027-03-14 03:30 (EDT)", ctx.toEpoch(wall(2027, 3, 14, 3, 30, 0)), utc(2027, 3, 14, 7, 30, 0));

    // 3b. A transition on a half hour: Asia/Colombo 1996-10-26 00:30 local (+06:30 -> +06:00, clocks
    // back to 00:00). 23:45 is unambiguously before the shift; 00:45 only exists after it. An
    // hour-bucket cache primed on the 00:xx hour would reuse the pre-shift offset for 00:45.
    // (00:00-00:29 occurred twice that night; such wall times are ambiguous and not tested.)
    setenv("TZ", "Asia/Colombo", 1);
    tzset();
    ctx.hostTimezoneName = "Asia/Colombo";
    ctx.hostTimezoneCache = UINT64_MAX;
    check("Colombo 1996-10-25 23:45 (+06:30)", ctx.toEpoch(wall(1996, 10, 25, 23, 45, 0)), utc(1996, 10, 25, 17, 15, 0));
    check("Colombo 1996-10-26 00:45 (+06:00)", ctx.toEpoch(wall(1996, 10, 26, 0, 45, 0)), utc(1996, 10, 25, 18, 45, 0));
    check("Colombo 1996-10-26 00:46 (+06:00)", ctx.toEpoch(wall(1996, 10, 26, 0, 46, 0)), utc(1996, 10, 25, 18, 46, 0));
    setenv("TZ", "America/New_York", 1);
    tzset();
    ctx.hostTimezoneName = "America/New_York";
    ctx.hostTimezoneCache = UINT64_MAX;

    // 4. Cache: same hour repeatedly, then a different hour, then back.
    check("cache same hour", ctx.toEpoch(wall(2026, 7, 1, 12, 59, 59)), utc(2026, 7, 1, 16, 59, 59));
    check("cache next hour", ctx.toEpoch(wall(2026, 7, 1, 13, 0, 0)), utc(2026, 7, 1, 17, 0, 0));
    check("cache winter again", ctx.toEpoch(wall(2026, 1, 15, 12, 0, 1)), utc(2026, 1, 15, 17, 0, 1));

    // 5. Timing: 10M calls, monotonically increasing seconds (the real pattern), both modes.
    const int N = 10000000;
    volatile time_t sink = 0;
    for (int mode = 0; mode < 2; ++mode) {
        ctx.hostTimezoneName = (mode == 0) ? "" : "America/New_York";
        ctx.hostTimezoneCache = UINT64_MAX;
        const uint32_t base = wall(2026, 7, 1, 0, 0, 0).getVal();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i)
            sink += ctx.toEpoch(Time(base + static_cast<uint32_t>(i % 86400)));   // one mktime per minute of redo time
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("timing %s: %.1f ns per call\n", mode == 0 ? "fixed offset" : "named zone (cached)", ns);
    }

    std::printf(failures == 0 ? "timezone test passed\n" : "timezone test FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
