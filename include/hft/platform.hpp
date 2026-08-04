#pragma once

#include <cstdint>

#include "hft/types.hpp"

// =============================================================================
// Platform abstraction.
//
// This project is developed on macOS (Apple Silicon) and performance-tested on
// Linux (x86_64). The two platforms differ in exactly the places that matter
// for an HFT engine: the cheapest monotonic clock, CPU pinning, and huge-page
// APIs. Rather than sprinkle #ifdefs through the hot path, isolate every such
// difference behind a thin interface here.
//
// Rules:
//   - Everything in this header MUST compile on both platforms.
//   - Prefer a portable-but-good implementation over a platform-specific one
//     unless you have MEASURED that the difference matters.
//   - Anything Linux-only (perf counters, hugepages, isolcpus) belongs in the
//     Linux perf harness, not here.
// =============================================================================

#if defined(__APPLE__)
#  define HFT_PLATFORM_MACOS 1
#elif defined(__linux__)
#  define HFT_PLATFORM_LINUX 1
#else
#  error "Unsupported platform: HFT core targets macOS and Linux only."
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define HFT_ARCH_X86_64 1
#include <x86intrin.h>
#elif defined(__aarch64__) || defined(__arm64__)
#  define HFT_ARCH_ARM64 1
#endif

#include <chrono>

namespace hft::platform {

// --- Monotonic nanosecond clock --------------------------------------------
// Returns a steady, monotonic timestamp in nanoseconds. This is the ONE clock
// the engine should use for latency measurement and timeouts.
//
// std::chrono::steady_clock is backed by mach_absolute_time on macOS and
// clock_gettime(CLOCK_MONOTONIC) on Linux. It is portable, monotonic, and the
// engine's general-purpose clock: timeouts, calibration, anything not on the
// per-message hot path.
//
// Latency measurement does NOT use it. Its read overhead (~20-30ns) is the
// same order as the work being timed, so the harness reads the cycle counter
// directly via read_cycles() below. now_ns() calibrates that counter in
// cycles_per_sec().
[[gnu::always_inline]] inline nanos_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[gnu::always_inline]] inline uint64_t read_cycles() noexcept {
#if defined(HFT_ARCH_ARM64)
    uint64_t t;
    asm volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#elif defined(HFT_ARCH_X86_64)
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#endif
}

inline uint64_t cycles_per_sec() noexcept {
#if defined(HFT_ARCH_ARM64)
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#elif defined(HFT_ARCH_X86_64)
    // no frequency register -> calibrate against the ns clock
    uint64_t c0 = read_cycles();
    nanos_t n0 = now_ns();
    while (now_ns() - n0 < 100'000'000) {}
    uint64_t c1 = read_cycles();
    nanos_t n1 = now_ns();
    // cycles per second = elapsed cycles / elapsed seconds
    return (c1 - c0) * 1'000'000'000ull / static_cast<uint64_t>(n1 - n0);
#endif
}

inline double cycles_to_ns(uint64_t cycles, uint64_t freq) noexcept {
    return static_cast<double>(cycles) * 1e9 / static_cast<double>(freq);
}

// --- CPU affinity -----------------------------------------------------------
// Pin the calling thread to a single core. This is essential for reproducible
// latency numbers, but is a no-op on macOS: Apple does not expose thread-to-core
// pinning to user space (thread_policy_set with affinity tags is only a hint and
// is ignored on Apple Silicon). So this returns false on macOS by design.
//
// On Linux it uses pthread_setaffinity_np. Real perf runs happen on Linux, so
// this being a no-op on the Mac is fine — you develop on Mac, you MEASURE on
// Linux.
//
// Returns true if the thread was actually pinned.
bool pin_thread_to_core(int core_id) noexcept;

// Human-readable platform tag, e.g. "macOS/arm64" or "Linux/x86_64".
// Handy to stamp into benchmark output so numbers are never misattributed.
const char* platform_tag() noexcept;

}  // namespace hft::platform
