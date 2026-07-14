#include <gtest/gtest.h>

#include "hft/platform.hpp"

// Verifies the platform abstraction compiles and behaves sanely on whatever
// host is running the tests (macOS dev box or Linux perf box / CI).

TEST(Platform, ClockIsMonotonic) {
    const hft::nanos_t a = hft::platform::now_ns();
    const hft::nanos_t b = hft::platform::now_ns();
    EXPECT_GE(b, a);
}

TEST(Platform, TagIsKnown) {
    // Should never be "unknown" on a supported platform.
    EXPECT_STRNE(hft::platform::platform_tag(), "unknown");
}

TEST(Platform, PinToCoreDoesNotCrash) {
    // On macOS this returns false (no user-space pinning); on Linux it may
    // return true. Either way it must not crash. We don't assert the result
    // because it's platform- and permission-dependent.
    (void)hft::platform::pin_thread_to_core(0);
}
