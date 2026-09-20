/* Data::epochNanoToString: the digits behind every nanosecond epoch OLR emits (timestamp 0,
 * timestamp-metadata 0, timestamp-tz 0 and 12). Exact for values beyond int64 nanoseconds
 * and for negative seconds with a fraction.
 * Build: part of -DWITH_TESTS=ON (target test_epoch_nano). */
#include <cstdio>
#include <cstring>
#include <string>
#include "../src/common/types/Data.h"

using namespace OpenLogReplicator;

namespace {
    int failures = 0;
    void check(int64_t seconds, uint64_t fraction, const char* want) {
        char buffer[40];
        const uint64_t n = Data::epochNanoToString(seconds, fraction, buffer);
        const std::string got(buffer, n);
        if (got == want)
            std::printf("PASS %ld.%09lu -> %s\n", static_cast<long>(seconds), static_cast<unsigned long>(fraction), want);
        else {
            std::printf("FAIL %ld.%09lu -> got %s want %s\n", static_cast<long>(seconds), static_cast<unsigned long>(fraction), got.c_str(), want);
            ++failures;
        }
    }
}

int main() {
    // int64 range, the common case
    check(0, 0, "0");
    check(0, 1, "1");
    check(1758240000, 123456789, "1758240000123456789");        // 2026, 19 digits
    check(999999999, 999999999, "999999999999999999");           // last value of the fast path
    check(1000000000, 0, "1000000000000000000");                 // first value of the split path
    check(1000000000, 1, "1000000000000000001");
    check(9223372036, 854775807, "9223372036854775807");         // INT64_MAX in nanos (2262-04-11)
    check(9223372036, 854775808, "9223372036854775808");         // one past it, beyond int64
    check(253402300799, 999999999, "253402300799999999999");     // 9999-12-31 23:59:59.999999999
    // negative: before 1970
    check(-1, 0, "-1000000000");
    check(-5, 500000000, "-4500000000");                         // 1969-12-31 23:59:55.5
    check(-999999999, 500000000, "-999999998500000000");         // fast path, negative with fraction
    check(-1000000000, 0, "-1000000000000000000");               // split path boundary
    check(-1000000000, 500000000, "-999999999500000000");        // the reported case
    check(-1000000001, 1, "-1000000000999999999");
    check(-62135596800, 0, "-62135596800000000000");             // 0001-01-01
    check(-210866803200, 0, "-210866803200000000000");           // -4712-01-01, Oracle's minimum
    check(-210866803200, 1, "-210866803199999999999");

    std::printf(failures == 0 ? "epoch-nano test passed\n" : "epoch-nano test FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
