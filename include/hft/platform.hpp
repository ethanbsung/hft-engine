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
// clock_gettime(CLOCK_MONOTONIC) on Linux. It is portable, monotonic, and good
// enough to start with. If/when you measure that its read overhead dominates
// (typically only matters at the ns level on the hottest path), swap in a
// TSC-based reader here behind the same signature — WITHOUT changing callers.
//
//   TODO(perf): consider rdtsc (__builtin_readcyclecounter / __rdtsc) on
//   x86_64 and cntvct_el0 on arm64, calibrated to ns. Only after measuring.
[[gnu::always_inline]] inline nanos_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
