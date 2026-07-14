# hft-engine

A clean-slate HFT engine, written to *learn* low-latency C++ and HFT
architecture end to end: real market data protocol parsing (NASDAQ ITCH
5.0), a production-shaped limit order book, and the performance discipline
(measured latency, zero hot-path allocation) that a real low-latency
trading system requires.

## Layout

```
include/hft/   public headers
src/           engine internals
tests/         GoogleTest unit tests
bench/         microbenchmarks (latency budget work)
docs/          design notes per component
main.cpp       entry point
```

## Build

The wrapper script picks sane defaults for whichever machine you're on:

```bash
./build.sh            # release build + run tests
./build.sh debug      # ASan/UBSan build + tests
./build.sh bench      # compile microbenchmarks (run them on Linux)
./build.sh perf       # Linux-only: -march=native + LTO for real perf runs
./build.sh clean
```

Or drive CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hft_system
ctest --test-dir build --output-on-failure
```

Useful options: `-DHFT_NATIVE=OFF` (portable binary, no `-march=native`),
`-DHFT_LTO=OFF`, `-DHFT_BUILD_BENCH=ON`.

## Cross-platform workflow: develop on macOS, measure on Linux

This project is **developed on macOS (Apple Silicon)** and
**performance-tested on Linux (x86_64)**. Those are deliberately different
roles:

- **The Mac is for writing code and checking correctness.** Latency numbers on
  macOS are *not* meaningful — there's no user-space core pinning, no way to
  disable frequency scaling, and it's a different ISA. Use it to build, run the
  tests, and compile the benchmarks (so they don't rot).
- **The Linux box is for real measurement.** `./build.sh perf` turns on
  `-march=native` + LTO; then pin a core and run:
  `taskset -c 2 ./build-perf/bench_clock`.

Everything platform-specific is isolated in **`include/hft/platform.hpp`** +
`src/platform.cpp` — the monotonic clock (`now_ns()`), core pinning
(`pin_thread_to_core`, a no-op on macOS), and a `platform_tag()` that stamps
every benchmark so a Mac number is never confused for a Linux one. Keep new
`#ifdef`s out of the hot path; add them here instead.

**CI (`.github/workflows/core-ci.yml`) builds + tests on Linux *and* macOS on
every push.** This is the safety net for the weeks only the Mac is available:
if a change breaks the Linux build (missing header, GCC-vs-Clang difference,
bad `#ifdef`), it surfaces immediately.

Debug build turns on ASan + UBSan (CI runs this on Linux too):
```bash
./build.sh debug
```

## Design notes

Per-component design writeups live in `docs/` — order book internals,
feed handler, ring buffer, threading model, and more.

## Ground rules for this project

- Decide data representations **before** writing the structures that use them.
- Don't claim "lock-free" / "sub-microsecond" in a comment until it's measured.
- One module at a time, each with tests, each measured before optimizing.
