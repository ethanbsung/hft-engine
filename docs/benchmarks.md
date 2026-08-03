# Benchmarks & Profiling — Results and Methodology

What was measured, how, and on what. No latency number appears anywhere in this
project that was not produced by the procedure below.

Companion docs: `latency-harness.md` (design of the measurement seam),
`design-decisions.md` (the design decisions behind the engine).

---

## Summary

Per-message **dispatch + apply** — one framed ITCH payload decoded and applied
to the order book — over **862 057 applied messages** across 7 symbols,
**20 independent runs** (4 invocations × 5):

| Percentile | Median | Range across runs |
|---|---|---|
| **p50** | **123 ns** | 120.24 – 126.18 |
| **p99** | **426 ns** | 423.08 – 431.26 |
| **p99.9** | **590 ns** | 577.47 – 601.98 |

Instrumentation overhead is **34–36 cycles (12.6–13.4 ns)**, included in every
figure above — the engine's own cost is ≈110 ns at p50.

Throughput, separately: **13.3 ns/msg, 74.8 M msgs/sec** (`bench_feed`).

**Conditions:** Intel Xeon Platinum 8280 @ 2.70 GHz (Cascade Lake), Ubuntu 24.04
/ 6.8.0-124-generic, DigitalOcean CPU-Optimized 2 vCPU, **KVM guest — not bare
metal**; `taskset -c 1`; replay from a local file, not wire-to-book; pre-market
flow. §5 lists what these numbers do not claim.

Raw output: `bench/results/vm_results_after.txt` (pre-fix run kept in
`vm_results_before.txt`).

---

## 1. What is measured

**Metric:** per-message **dispatch + apply** — taking one already-framed ITCH
payload and mutating the order book: message-type dispatch, field decode, and
the book operation (`add`/`delete`/`execute`/`cancel`/`replace`).

**Off the clock, deliberately:**
- File I/O — the fixture is read into a `std::vector` once, before timing.
- Framing (`[u16 len][payload]` splitting) — measured separately as throughput,
  not folded into per-message latency.
- Network receive — there is no NIC; ITCH replay is market-data-only.

| Harness | Binary | Reports |
|---|---|---|
| Latency distribution | `bench_latency` | p50 / p99 / p99.9 per-message, median + range over 5 runs |
| Throughput | `bench_feed` | ns/msg and msgs/sec, min-of-N |

**Why throughput (13.3 ns) is lower than latency p50 (123 ns).** `bench_feed`
times *all* 16.3 M framed messages; ~99% are non-watchlist symbols that hit
`books.get(locate) → nullptr → return` and early-out, pulling the average down.
`bench_latency` times *only* the 862 k **applied** messages that mutate a book.
Different denominators. The throughput figure mostly measures the framing loop,
so p50 is the meaningful per-operation number.

---

## 2. How the measurement is taken

- **Cycle counter, not `clock_gettime`.** Per-message work is tens of ns;
  `clock_gettime` read overhead (~20–30 ns) would dominate. Reads the CPU cycle
  counter directly — `rdtsc` on x86_64, `cntvct_el0` on arm64
  (`platform::read_cycles()`) — converting ticks→ns with a calibrated frequency
  (`cycles_per_sec()`).
- **The `rdtsc` read is fenced.** `rdtsc` is not serializing, so the CPU can
  reorder it around the code being timed. `read_cycles()` wraps it
  `lfence; rdtsc; lfence`, bracketing exactly the `apply()` region. (The ARM
  path is unfenced; measurement happens on x86.)
- **Instrumentation compiles out.** The decode path is templated on
  `<bool Timing>`. `decode<false>` (production) removes the counter reads and
  `LatencySink` call via `if constexpr` — absent from the emitted code, not
  branched around. Confirmed by unchanged throughput between the two.
- **Overhead quantified, not subtracted.** An empty fenced bracket measures
  34–36 cycles (12.62 / 13.36 ns): eight samples, every one landing on exactly
  one of those two values. Percentiles are reported raw with that floor included.
- **Warm-up pass.** A full `decode<false>` pass runs before timing to warm
  caches and branch predictors and fault in pages, so timed runs measure steady
  state.
- **Fresh book per run.** Each run builds a new `BookSet`; state does not leak
  between runs.
- **Median of per-run percentiles, plus range.** Each run yields a full
  distribution; the reported figure is the median of the per-run p50/p99/p99.9
  with the min–max spread across runs. Raw samples are never pooled across runs
  — pooling would conceal the run-to-run variance the repetition exists to
  expose.
- **`do_not_optimize` barrier.** An asm barrier
  (`asm volatile("" : : "r,m"(v) : "memory")`) keeps the decode result live so
  the optimizer cannot delete the work, without `volatile`'s store cost.

**Sample size.** ~862 k applied messages per run, across a 7-symbol watchlist
(SPY, QQQ, MSFT, AAPL, TSLA, GOOGL, AMZN) — a single symbol does not produce
enough applied events to populate the tail. p99.9 needs ~1 k samples in the top
0.1%; 862 k gives ~862, which supports p99.9 but not p99.99.

---

## 3. Environment

Development happens on macOS (Apple Silicon); **measurement happens on
Linux/x86_64**. The Mac's `cntvct_el0` ticks at 24 MHz (~42 ns/tick), so sub-42
ns work floors to zero. Resolving per-message latency requires the x86 TSC.

- **Instance:** DigitalOcean CPU-Optimized, 2 vCPU / 4 GB.
- **CPU:** Intel Xeon Platinum **8280** @ 2.70 GHz (Cascade Lake). Calibrated
  TSC 2.694–2.695 GHz, matching the nameplate clock. The earlier pre-fix run
  landed on a Xeon **8168** (Skylake-SP) — see the caveat in §4.
- **OS:** Ubuntu 24.04 LTS, 6.8.0-124-generic.
- **Virtualization:** KVM guest (`pc-i440fx`), not bare metal.

**TSC validity.** `constant_tsc` is **present** — the TSC ticks at a fixed rate
regardless of core frequency, which is the flag that governs whether ticks→ns is
stable. `nonstop_tsc` is **absent**, meaning the TSC could stop in deep idle;
this does not affect the benchmark, because the timed loop is CPU-bound on a
busy pinned core that never idles. The guest exposes no `cpufreq`, so no
governor can be set; `constant_tsc` plus a pinned busy core makes that moot.

**Pinning.** `taskset -c 1` pins the benchmark to core 1, leaving the OS on core
0. The workload is single-threaded, so one dedicated core is sufficient
isolation.

### Reproduce
```bash
# on the Linux/x86 box:
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u   # expect constant_tsc
./build.sh perf                    # -O3 -march=native -flto -fno-omit-frame-pointer
taskset -c 1 ./build-perf/bench_latency
taskset -c 1 ./build-perf/bench_feed
```

---

## 4. Hardware counters and profile

`perf stat` on `bench_feed`:

```
instructions     17_817_911_865
cache-references    282_157_815
cache-misses        206_164_332   # 73.07% of all cache refs
branches          3_488_380_602
branch-misses        42_304_746   # 1.21% of all branches
```

**The 73% cache-miss rate is expected, not a locality bug.** `bench_feed`
streams a 500 MB buffer once, linearly, with no reuse, so nearly all misses are
compulsory. The hardware prefetcher hides the latency — throughput holds at
74.8 M/s. Branch prediction is healthy at 1.21%.

**`cycles` reads as 0** on this KVM guest: the cycle PMU is gated even though
instruction counting works. IPC therefore cannot be computed from this data and
is not claimed.

Counters from `bench_latency` are in the raw output but are less useful for
judging the engine — that binary also runs the 200 k-iteration overhead loop and
five `std::sort`s of 862 k samples, inflating its branch-miss rate to 2.17%.

`perf record` self view (`bench_latency`, frame-pointer unwinding):

```
main                  43.9%   (decode loop + dispatch, inlined)
add_order             13.2%   (book mutation)
std::__introsort_loop  7.8%   (std::sort of samples — bench bookkeeping,
                               outside the timed region, not engine cost)
```

**Two things in this profile are easy to misread.** The call graph shows a
page-fault chain under `OrderBook::OrderBook` — real, but it is the kernel
committing each book's ~4 MB of arrays on first touch, and it is **not in the
histogram**: that path runs from the `R` (directory) branch of `decode_message`,
which returns *before* the `if constexpr (Timing)` block. It is startup cost.
Separately, `recenter()` had no frame of its own because it inlined into
`add_order` under LTO — so the profile could not show the function that was
responsible for the tail (§5).

### Effect of the fixed-window fix

The recentre path documented in the README was removed by replacing the sliding
price window with a fixed one. Measured on the same fixture and harness:

| | before | after | |
|---|---|---|---|
| **p99.9** | 15 981 ns | **590 ns** | **27× lower** |
| p99 | 427 ns | 426 ns | unchanged |
| p50 | ~120 ns | 123 ns | unchanged |
| samples ≥ 5 µs | ~2 150/run | ~225/run | 10× fewer |
| recentres | 2 124 | **0** | eliminated |
| orders evicted | ~14 000 | **0** | eliminated |
| throughput | 14.0 ns/msg | 13.3 ns/msg | see caveat |
| instructions | 23.9 B | 17.8 B | 25% fewer |

p50 and p99 unchanged is the expected signature: the fix removed a rare
expensive path without altering the common-path instruction sequence.
`add_order` self time fell from 19.5% to 13.2% as the recentre path left the hot
function.

> **Caveat on the throughput comparison.** DigitalOcean provisioned a Xeon 8168
> for the pre-fix run and a Xeon 8280 for the post-fix run — same 2.70 GHz
> nameplate, different microarchitecture. Part of the ~5% throughput gain may be
> the newer silicon, so it is not claimed outright. The instruction-count drop
> is hardware-independent and is attributable. The 27× tail reduction is far too
> large for a CPU difference to explain.

The residual ~225 samples ≥ 5 µs per run (0.026% of 862 k) are OS scheduling
noise. Running the same binary under `perf record` raised that count to ~730/run
— the profiler's own sampling interrupts — while p50 and p99 barely moved.

---

## 5. How the tail was attributed

The README describes both bugs the benchmark surfaced. What belongs here is the
evidence that settled attribution, since the first hypothesis was wrong and the
profile reinforced it.

**`perf` named the wrong cause.** Its call graph pointed at the page-fault chain
under the book constructor, which looked conclusive. It was not: that path
returns before the timing block, so book construction was never sampled into the
histogram. The faults were real and irrelevant. An early version of this document
asserted they were the tail and stated it as settled; that is left on the record
rather than quietly edited out, because the failure mode — trusting a profile
without checking whether the code it names is inside the timed region — is more
instructive than the answer.

**Direct counter instrumentation is what proved it.** Counting samples ≥ 5 µs
against the book's own `recenters_` counter, per run:

| | run 0 | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|---|
| samples ≥ 5 µs | 2243 | 2162 | 2213 | 2155 | 2146 |
| total recenters | — | — | — | — | **2124** |

~1 tail sample per recentre, within 1–4%. No profile could have shown this,
because `recenter` inlined into `add_order` and had no frame of its own.

**Two facts independently ruled out the page-fault explanation:**

1. **The tail reproduced on macOS/arm64** (~8 µs, same 2124 recentres) — a
   different kernel, allocator, and page-fault path. A first-touch-fault
   explanation does not survive an OS change; a fixed-cost scan does.
2. **The tail was tight** — 15.9–16.1 µs on x86, 8.0–8.4 µs on arm64. Page
   faults jitter; `rebuild_bitmap()` was a constant 2 × 4096-level sequential
   scan (~192 KB), the shape of a fixed-cost operation. The ~2× gap between
   platforms tracks memory bandwidth, not fault cost.

For the second bug, the same principle applied in a different form: an
independent Python scan of the raw ITCH bytes established that the feed
contained zero references to unknown orders, which located the fault in the book
rather than the data.

**Book health on the same runs that produced the timings:** `not_found = 0`
across all 7 symbols, no crossed books. Far orders (ITCH's \$199,999.99
sentinel, penny stubs, genuinely deep passive orders) total 1 368, dominated by
TSLA's 821.

---

## 6. What this run does not claim

- **Not bare metal** — KVM guest. A real desk measures on tuned bare metal with
  isolcpus, hugepages, and NIC kernel-bypass.
- **No IPC** — the `cycles` PMU reads 0 on this guest, so instructions-per-cycle
  cannot be computed.
- **No p99.99** — the sample count supports p99.9, not a stable p99.99 off a
  single day's slice.
- **Replay, not live** — no NIC receive, no wire-to-book. The figure is the
  engine's own dispatch+apply cost.
- **Pre-market data** — the fixture spans 04:00–09:30 ET, so message rates and
  book depth are not representative of the full session.
- **No comparison to industry figures.** Firms do not publish per-message
  book-apply latency. The figures that circulate publicly — sub-microsecond,
  commonly 1–5 µs — are almost always **tick-to-trade** (NIC in → decision → NIC
  out), covering network stack, decode, book, strategy, risk, and encoding. The
  123 ns here is one stage inside that path. The genuinely fast book
  implementations are FPGAs, at tens of nanoseconds — a hardware comparison, not
  a software one.
- **No optimisation pass** — everything measured so far removed a pathology;
  nothing has yet targeted the common path.

**On the magnitude.** ~110 ns at 2.694 GHz is ~300 cycles for a hash probe, a
level access, a list unlink, and a three-tier bitmap update. An L3 hit is ~40
cycles and a DRAM miss ~200–300, so that budget is consistent with a couple of
cache misses plus real work, and the 73% cache-miss rate corroborates it. That
reasoning rests on hardware behaviour rather than on an unpublished benchmark.

---

## 7. Open items

1. **An optimisation pass on the common path.** `add_order` is 13% of self time
   and the cache-miss rate is 73% — the ref-index probe is the obvious first
   suspect (layout, prefetch, or a cheaper mix).
2. **A full-session fixture.** The current one ends at 09:30 ET, so the
   end-of-day invariant (book empty after the closing `S` message) cannot be
   asserted.
3. **A staleness signal.** `not_found_` is currently a counter; in a live system
   it would trip a per-symbol health flag that halts quoting and triggers a
   GLIMPSE re-snapshot.
4. **Bare-metal comparison.** A tuned run with isolcpus and hugepages would show
   how much of the tail is the hypervisor.
