#pragma once

// do_not_optimize: keep a value "live" across an inline-asm barrier so the
// optimizer cannot elide the code that produced it, WITHOUT the memory-store
// cost of `volatile`. This is Google Benchmark's DoNotOptimize primitive.
// "r,m"(v) forces v into a register or memory; "memory" clobber prevents the
// compiler reordering/dropping surrounding memory ops. GCC/Clang only.
template <typename T>
static inline void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}