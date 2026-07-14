// Microbenchmark: cost of hft::platform::now_ns().
//
// This is the reference example for how bench/ works. Every benchmark:
//   - stamps hft::platform::platform_tag() so a number is never misattributed
//     to the wrong machine (Mac numbers != Linux numbers);
//   - is built only with -DHFT_BUILD_BENCH=ON;
//   - is only MEANINGFUL on the Linux perf box (pinned core, no frequency
//     scaling), but must COMPILE on the Mac so breakage surfaces immediately.
//
// Run real measurements on Linux with the core pinned:
//   taskset -c 2 ./core/build/bench_clock

#include <cstdint>
#include <cstdio>

#include "hft/platform.hpp"

int main() {
    constexpr int kIters = 10'000'000;

    // Warm up.
    volatile hft::nanos_t sink = 0;
    for (int i = 0; i < 1'000'000; ++i) sink = hft::platform::now_ns();

    const hft::nanos_t start = hft::platform::now_ns();
    for (int i = 0; i < kIters; ++i) {
        sink = hft::platform::now_ns();
    }
    const hft::nanos_t end = hft::platform::now_ns();
    (void)sink;

    const double ns_per_call = static_cast<double>(end - start) / kIters;
    std::printf("[%s] now_ns(): %.2f ns/call over %d iters\n",
                hft::platform::platform_tag(), ns_per_call, kIters);
    return 0;
}
