/* Checks Ctx::toEpoch: fixed-offset mode is unchanged, and a named host zone resolves the
 * UTC offset per wall-clock minute across DST transitions, including the minute cache and the
 * repeated fall-back hour (resolved from the redo order through Ctx::noteRedoTime).
 * Also times both modes so a regression in the hot path shows up.
 *
 * Build: part of -DWITH_TESTS=ON (target test_timezone). Needs /usr/share/zoneinfo. */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "../src/common/Ctx.h"
#include "../src/common/types/Data.h"
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

    // 0. Fixed offsets (host-timezone, db-timezone, OLR_LOG_TIMEZONE): hours and two-digit minutes,
    // the sign applies to the whole offset; Etc/GMT+N is UTC-N (POSIX convention).
    {
        const struct { const char* text; bool ok; int64_t seconds; } cases[] = {
            {"-04:00", true, -4 * 3600}, {"+05:30", true, 5 * 3600 + 1800}, {"+10:00", true, 10 * 3600},
            {"-03:30", true, -(3 * 3600 + 1800)}, {"+5:45", true, 5 * 3600 + 2700}, {"+14:00", true, 14 * 3600},
            {"-10:00", true, -10 * 3600}, {"+00:00", true, 0}, {"-00:00", true, 0},
            {"Etc/GMT-5", true, 5 * 3600}, {"Etc/GMT+5", true, -5 * 3600}, {"EST", true, -5 * 3600}, {"UTC", true, 0},
            {"+05:60", false, 0}, {"+19:00", false, 0}, {"05:00", false, 0}, {"+5", false, 0}, {"+05-30", false, 0},
        };
        for (const auto& c: cases) {
            int64_t got = 12345;
            const bool ok = Data::parseTimezone(c.text, got);
            if (ok == c.ok && (!ok || got == c.seconds))
                std::printf("PASS parse %s\n", c.text);
            else {
                std::printf("FAIL parse %s: ok=%d got %ld want ok=%d %ld\n", c.text, ok, static_cast<long>(got), c.ok,
                            static_cast<long>(c.seconds));
                ++failures;
            }
        }
    }

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

    // 3c. Fall-back night in New York as redo delivers it: 01:00-01:59 happens twice. The first
    // pass is EDT (-4), the second EST (-5); e_tm must follow real time and never go back.
    // As the parser does: every LWN timestamp goes through noteRedoTime, the marked value is
    // what transactions store and builders convert
    const auto read = [&ctx](Time t) { return ctx.noteRedoTime(t); };
    const auto redo = [&ctx, &read](Time t) { return ctx.toEpoch(read(t)); };
    const auto resetFold = [&ctx]() {
        ctx.lastRedoWall = INT64_MIN;
        ctx.foldWallStart = INT64_MIN;
        ctx.foldWallEnd = INT64_MIN;
        ctx.hostTimezoneCache = UINT64_MAX;
    };
    resetFold();
    check("fold 00:59:59 EDT", redo(wall(2026, 11, 1, 0, 59, 59)), utc(2026, 11, 1, 4, 59, 59));
    check("fold 01:30:00 first pass", redo(wall(2026, 11, 1, 1, 30, 0)), utc(2026, 11, 1, 5, 30, 0));
    // a one-second clock step back inside the repeated hour is not a fold
    check("fold 01:29:59 step back, still first pass", redo(wall(2026, 11, 1, 1, 29, 59)), utc(2026, 11, 1, 5, 29, 59));
    check("fold 01:59:59 first pass", redo(wall(2026, 11, 1, 1, 59, 59)), utc(2026, 11, 1, 5, 59, 59));
    check("fold 01:00:00 second pass", redo(wall(2026, 11, 1, 1, 0, 0)), utc(2026, 11, 1, 6, 0, 0));
    check("fold 01:30:00 second pass", redo(wall(2026, 11, 1, 1, 30, 0)), utc(2026, 11, 1, 6, 30, 0));
    // a log switch in the second pass: the header's next-time follows the LWNs read so far
    check("fold log switch 01:40 second pass", ctx.toEpoch(ctx.markRedoTime(wall(2026, 11, 1, 1, 40, 0))), utc(2026, 11, 1, 6, 40, 0));
    check("fold 01:59:59 second pass", redo(wall(2026, 11, 1, 1, 59, 59)), utc(2026, 11, 1, 6, 59, 59));
    check("fold 02:00:00 EST", redo(wall(2026, 11, 1, 2, 0, 0)), utc(2026, 11, 1, 7, 0, 0));
    // a time not read through the parser (no mark) is the first occurrence
    check("fold unmarked 01:15", ctx.toEpoch(wall(2026, 11, 1, 1, 15, 0)), utc(2026, 11, 1, 5, 15, 0));
    // the next year's fall-back starts undetected again: its first pass is EDT
    check("fold 2027-11-07 01:30 first pass", redo(wall(2027, 11, 7, 1, 30, 0)), utc(2027, 11, 7, 5, 30, 0));

    // A log switch right after the clocks went back, before any LWN of the second pass: the
    // header's next-time is the first time of the second pass and is what detects it
    resetFold();
    check("fold switch: last LWN 01:59:00 first pass", redo(wall(2026, 11, 1, 1, 59, 0)), utc(2026, 11, 1, 5, 59, 0));
    check("fold switch: header 01:00:10 second pass", ctx.toEpoch(ctx.markRedoTime(wall(2026, 11, 1, 1, 0, 10))),
          utc(2026, 11, 1, 6, 0, 10));
    check("fold switch: next LWN 01:00:20 second pass", redo(wall(2026, 11, 1, 1, 0, 20)), utc(2026, 11, 1, 6, 0, 20));
    // ... followed by an idle hour: the next LWN is later than the last one of the first pass, so
    // only the switch shows that the clocks went back (the parser marks it with or without metrics)
    resetFold();
    (void)read(wall(2026, 11, 1, 1, 59, 0));
    (void)ctx.markRedoTime(wall(2026, 11, 1, 1, 0, 10));
    check("fold switch, idle hour: next LWN 01:59:30 second pass", redo(wall(2026, 11, 1, 1, 59, 30)), utc(2026, 11, 1, 6, 59, 30));

    // A transaction buffered across the change: its begin is read in the first pass, its commit in
    // the second, and both are converted only at commit. The begin keeps its first occurrence.
    resetFold();
    {
        const Time begin = read(wall(2026, 11, 1, 1, 30, 0));     // 01:30 EDT
        (void)read(wall(2026, 11, 1, 1, 59, 50));
        const Time commit = read(wall(2026, 11, 1, 1, 10, 0));    // 01:10 EST, after the change
        const time_t beginEpoch = ctx.toEpoch(begin);
        const time_t commitEpoch = ctx.toEpoch(commit);
        check("buffered begin 01:30 EDT converted after the change", beginEpoch, utc(2026, 11, 1, 5, 30, 0));
        check("commit 01:10 EST", commitEpoch, utc(2026, 11, 1, 6, 10, 0));
        if (beginEpoch > commitEpoch) {
            std::printf("FAIL buffered begin later than its commit\n");
            ++failures;
        }
    }

    // Every 7 s from 00:50 EDT to 02:10 EST, as a busy database writes redo: never decreasing,
    // and the end lands on real time.
    resetFold();
    {
        time_t last = 0;
        bool monotonic = true;
        const time_t start = utc(2026, 11, 1, 4, 50, 0);
        const time_t end = utc(2026, 11, 1, 7, 10, 0);
        for (time_t real = start; real <= end; real += 7) {
            struct tm local{};
            localtime_r(&real, &local);
            const time_t got = redo(wall(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec));
            if (got != real || got < last) {
                std::printf("FAIL fold walk at real %ld: got %ld\n", static_cast<long>(real), static_cast<long>(got));
                monotonic = false;
                break;
            }
            last = got;
        }
        if (monotonic)
            std::printf("PASS fold walk 00:50 EDT -> 02:10 EST, e_tm equals real time and never decreases\n");
        else
            ++failures;
    }

    // Half-hour fold: Asia/Colombo 1996-10-26, 00:00-00:29 happened twice (+06:30 then +06:00).
    setenv("TZ", "Asia/Colombo", 1);
    tzset();
    ctx.hostTimezoneName = "Asia/Colombo";
    resetFold();
    check("Colombo fold 00:20 first pass", redo(wall(1996, 10, 26, 0, 20, 0)), utc(1996, 10, 25, 17, 50, 0));
    check("Colombo fold 00:29:59 first pass", redo(wall(1996, 10, 26, 0, 29, 59)), utc(1996, 10, 25, 17, 59, 59));
    check("Colombo fold 00:05 second pass", redo(wall(1996, 10, 26, 0, 5, 0)), utc(1996, 10, 25, 18, 5, 0));
    setenv("TZ", "America/New_York", 1);
    tzset();
    ctx.hostTimezoneName = "America/New_York";
    resetFold();

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
    {
        // The per-LWN hook in the parser: redo time moving forward, the common path
        ctx.hostTimezoneName = "America/New_York";
        ctx.lastRedoWall = INT64_MIN;
        const uint32_t base = wall(2026, 7, 1, 0, 0, 0).getVal();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i)
            sink += ctx.noteRedoTime(Time(base + static_cast<uint32_t>(i / 100))).getVal();
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("timing noteRedoTime (per LWN): %.1f ns per call\n", ns);
    }

    std::printf(failures == 0 ? "timezone test passed\n" : "timezone test FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
