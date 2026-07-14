#include "hft/platform.hpp"

// Platform-specific implementations of the interface declared in platform.hpp.
// Keep ALL the #ifdef'd system-call code confined to this file.

#if defined(HFT_PLATFORM_LINUX)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace hft::platform {

bool pin_thread_to_core(int core_id) noexcept {
#if defined(HFT_PLATFORM_LINUX)
    if (core_id < 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(core_id), &set);
    // Pin the calling thread. rc == 0 means success.
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    // macOS: no user-space core pinning. Intentional no-op.
    (void)core_id;
    return false;
#endif
}

const char* platform_tag() noexcept {
#if defined(HFT_PLATFORM_MACOS) && defined(HFT_ARCH_ARM64)
    return "macOS/arm64";
#elif defined(HFT_PLATFORM_MACOS) && defined(HFT_ARCH_X86_64)
    return "macOS/x86_64";
#elif defined(HFT_PLATFORM_LINUX) && defined(HFT_ARCH_X86_64)
    return "Linux/x86_64";
#elif defined(HFT_PLATFORM_LINUX) && defined(HFT_ARCH_ARM64)
    return "Linux/arm64";
#else
    return "unknown";
#endif
}

}  // namespace hft::platform
