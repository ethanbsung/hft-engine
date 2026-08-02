# hft-engine

A from-scratch low-latency market data engine: **Nasdaq TotalView-ITCH 5.0**
decoded into a production-shaped limit order book, with latency measured
honestly on a pinned x86 core.

> **p50 123 ns / p99 426 ns** per-message dispatch + apply, over **862 057
> applied ITCH messages** across 7 symbols, on a pinned core at 2.694 GHz.
> Instrumentation overhead (34–36 cycles) measured and **included** in those
> numbers; net engine cost ≈110 ns at p50.

Full methodology, the environment's limitations, and what these numbers
explicitly do *not* claim: **[docs/benchmarks.md](docs/benchmarks.md)**.

A learning project built to the bar of a real trading system rather than a
tutorial: real exchange protocol, real book semantics, measured latency, and
no performance claim that isn't backed by a run.

---

## The part worth reading: two bugs found by refusing to trust the numbers

Both were found *after* the code passed its unit tests and produced
plausible-looking latency, by sanity-checking the output instead of
publishing it.

**1. A p99.9 tail that was really a correctness bug.** The tail sat at
**15 981 ns** while p50 was ~120 ns. Profiling blamed `recenter()`'s bitmap
rebuild — but the real problem was that recentering existed at all: the price
window was being dragged by far-away orders, evicting live levels. Replacing it
with a fixed window cut **p99.9 by 27×** (15 981 ns → 590 ns) and 25% of
instructions, while p50 and p99 were unchanged — exactly the signature of
removing a rare expensive path rather than speeding up the common one.

**2. A hash-table `erase` that silently orphaned orders.** After the window fix,
**6 of 7 books ended crossed** — best bid above best ask, which is impossible.
Isolating it:

- Ruled out the data — a separate Python pass over the raw ITCH bytes, written
  against the spec independently of the C++ decoder, found 113 073 SPY adds,
  109 898 deletes, and **zero** unknown refs.
- Ruled out capacity — peak 3 177 resting orders against a 65 536 pool.
- **Differential fuzz** against `std::unordered_map` failed in **4 operations
  with 2 live entries**, small enough to reduce by hand.

Root cause: `RefIndex::erase` computed backward-shift distances with a lambda
capturing the hole index *by reference* while reassigning it in the loop, so
after the first shift it skipped entries needing relocation — and `find`, which
stops at the first empty slot, gave up early and orphaned the rest. It needs a
probe chain of ≥ 3 to manifest, which is why hand-written unit tests missed it.

Result: **6 of 7 books crossed → 0**, `not_found` **543 → 0**, matching the
Python ground truth exactly. Regression coverage: 6 collision-chain tests plus a
differential fuzz checking every live entry after each of 20 000 operations.

---

## Measurement discipline

Latency numbers are only as good as the conditions they were taken under, so
those are stated up front rather than buried:

- **Measured on Linux/x86_64, never on the Mac.** Apple Silicon's `cntvct_el0`
  ticks at 24 MHz (~42 ns), so sub-42 ns work floors to zero — useless for this.
  Every benchmark stamps `platform_tag()` so a Mac number can't be mistaken for
  a Linux one.
- **20 independent runs** (4 invocations × 5), reporting the **median of
  per-run percentiles plus the full range** — not an average of averages.
- **862 k applied messages** — enough samples in the top 0.1% to state p99.9
  with a straight face, and not enough to claim p99.99, so it isn't claimed.
- **Timer overhead quantified and disclosed, not subtracted.**
- **Honest about the ceiling:** a KVM guest, not tuned bare metal. No
  `isolcpus`, no hugepages, no kernel bypass. Replay, not wire-to-book.

[docs/benchmarks.md](docs/benchmarks.md) §7 lists what the run does not claim.

---

## What's built

```
                        ┌──── timed region (dispatch + apply) ────┐
  ITCH file  ──▶ framing ──▶ decode/dispatch ──▶ BookSet ──▶ OrderBook
  [u16 len]      (u16 BE      (switch on type    (locate→   ├─ slot pool (preallocated)
  [payload]       prefix)      byte, zero-copy    book)     ├─ ref→slot index (open-addressed)
                               BE field loads)              ├─ level array (fixed window)
                                                            └─ 3-tier touch bitmap
```

Everything above runs on one thread today; the timed region is what the p50/p99
numbers describe.

| Component | Status | What it does |
|---|---|---|
| **Feed handler** | ✅ | Length-prefixed ITCH 5.0 binary framing → decode of `A`/`F` add, `D` delete, `C`/`E` execute, `X` cancel, `U` replace, `P` trade, plus stock directory. Zero-copy big-endian loads, no allocation, no string parsing on the hot path. Replay off the binary file; MoldUDP64 transport and gap detection are not built. |
| **Order book** | ✅ | Per-symbol, order-by-order (not level-aggregated). Fixed price window, intrusive doubly-linked FIFO per level for price-time priority, preallocated slot pool, open-addressed ref→slot index. |
| **Touch lookup** | ✅ | Three-tier bitmap (summary → mid → bits) so best-bid/ask is a few bit-scan ops instead of a linear scan. |
| **Latency harness** | 🚧 | Compile-time-gated timing tap (`template<bool Timing>`) so instrumentation costs nothing when off; p50/p99/p99.9 reporting in `bench/`. |
| Ring buffer, strategy, risk, exec sim | ⬜ | Designed in `docs/design/`, not yet built — replay is single-threaded today. |

48 GoogleTest cases. CI builds and tests on **Linux and macOS** every push,
with an ASan/UBSan build on Linux.

The fixed price window is anchored per symbol at construction. Reference prices
are hardcoded for this replay (`src/book_set.cpp`); in production they would come
from the previous close.

---

## Build

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
ctest --test-dir build --output-on-failure
```

Useful options: `-DHFT_NATIVE=OFF` (portable binary, no `-march=native`),
`-DHFT_LTO=OFF`, `-DHFT_BUILD_BENCH=ON`.

> `hft_system` is a build smoke-test that prints and exits — there is no
> engine `main` yet. The real entry points are the benchmarks below, which
> drive the feed handler and book over a replay file.

### Getting the fixture

The benchmarks replay `tests/fixtures/itch_500m.bin` — a 500 MB slice of a real
Nasdaq TotalView-ITCH 5.0 session. Capture files are gitignored (a full day is
several GB), so fetch and slice one:

```bash
# Nasdaq publishes sample ITCH 5.0 days, plain HTTPS, no auth:
#   https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
mkdir -p tests/fixtures
curl -s 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302020.NASDAQ_ITCH50.gz' \
  | gunzip -c | head -c 500000000 > tests/fixtures/itch_500m.bin
```

Stream it rather than downloading first: the file is 5.6 GB compressed as a
**single** gzip stream, so range requests won't inflate standalone and the
decompressed day is far larger still. The format is a raw BinaryFILE stream
(`[u16 BE length][payload]`, back to back), so a byte-prefix slice starts at true
session open — the first ~2 MB is start-of-day directory messages before any
order flow appears.

The published numbers used **2020-01-30** (`01302020`). Any ITCH 5.0 day works,
but symbols and their reference prices are hardcoded in `src/book_set.cpp`, so a
different day needs different reference prices.

### Reproducing the measured numbers

```bash
# on the Linux/x86 box:
grep -o 'constant_tsc' /proc/cpuinfo | sort -u   # measurement validity depends on this
./build.sh perf
taskset -c 1 ./build-perf/bench_latency
taskset -c 1 ./build-perf/bench_feed
```

---

## Layout

```
include/hft/   public headers
src/           engine internals
tests/         GoogleTest unit tests
bench/         microbenchmarks (bench_latency, bench_feed, bench_clock)
               + results/ — raw output from the measured runs
docs/          design notes per component + benchmark methodology
docs/design/   design-time notes for components not yet built
dead/          retired Coinbase JSON feed — kept as a record of why it was dropped
main.cpp       build smoke-test, not the engine
```

**Develop on macOS, measure on Linux.** Everything platform-specific is
isolated in `include/hft/platform.hpp` + `src/platform.cpp` — the monotonic
clock (`now_ns()`), core pinning (`pin_thread_to_core`, a no-op on macOS), and
the `platform_tag()` stamp. New `#ifdef`s go there, never in the hot path.

CI (`.github/workflows/core-ci.yml`) is the safety net for stretches when only
the Mac is available: a change that breaks the Linux build — missing header,
GCC-vs-Clang difference, bad `#ifdef` — surfaces on push.

---

## Docs

Start with **[docs/benchmarks.md](docs/benchmarks.md)** (results + methodology)
and **[docs/design-decisions.md](docs/design-decisions.md)** (why the engine is
built the way it is, and what the alternatives cost).
**[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** covers the system shape and
module index. Per-component notes: [order book](docs/order-book.md),
[feed handler](docs/feed-handler.md), [types](docs/types.md),
[latency harness](docs/latency-harness.md).

**[docs/design/](docs/design/)** holds design-time notes for components that are
**not yet built** — ring buffer, threading, strategy, risk, execution simulator.
They describe intent, not shipped code.
