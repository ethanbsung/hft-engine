# Benchmarks & Profiling — Results and Methodology

The numbers this project is allowed to claim, and *exactly* how they were
produced. Per the README rule, no latency number is stated that was not
measured on the methodology below. This doc is the source of truth for
"what's the number" and the script for defending it in an interview.

Companion docs: `latency-harness.md` (design of the measurement seam),
`interview-prep.md` (design-decision cheat sheet).

---

## 1. What is measured

**Metric:** per-message **dispatch + apply** latency — the cost of taking one
already-framed ITCH payload and mutating the order book: message-type
dispatch, field decode, and the book operation (`add`/`delete`/`execute`/
`cancel`/`replace`). This is the hot path a real feed handler runs per
message.

**Not** measured / deliberately off-clock:
- File I/O (fixture is read into a `std::vector` once, before timing).
- Framing (`[u16 len][payload]` splitting) — timed separately by throughput,
  not folded into per-message latency.
- Network receive — there is no NIC; ITCH replay is market-data-only.

Two harnesses:
| Harness | Binary | Reports |
|---|---|---|
| Latency distribution | `bench_latency` | p50 / p99 / p99.9 per-message, median + range over 5 runs |
| Throughput | `bench_feed` | ns/msg and msgs/sec, min-of-N |

---

## 2. How the measurement is taken (and why it's honest)

- **Cycle counter, not `clock_gettime`.** Per-message work is tens of ns;
  `clock_gettime` read overhead (~20–30 ns) would dominate. We read the CPU
  cycle counter directly: `rdtsc` on x86_64, `cntvct_el0` on arm64
  (`platform::read_cycles()`), and convert ticks→ns with a calibrated
  frequency (`cycles_per_sec()`).
- **The rdtsc read is fenced.** `rdtsc` is not serializing; the CPU can
  reorder it around the code being timed. `read_cycles()` wraps it
  `lfence; rdtsc; lfence` so t0/t1 bracket exactly the `apply()` region and
  nothing leaks in or out of the window. (ARM path is unfenced — measurement
  happens on x86; documented in `platform.hpp`.)
- **Compile-time instrumentation, zero prod cost.** The decode path is
  templated on `<bool Timing>`. `decode<false>` (production) compiles the
  timing *out* via `if constexpr` — the counter reads and the `LatencySink`
  call don't exist in the emitted code, not merely branched around.
  `decode<true>` (the harness) compiles them in. Confirmed by unchanged
  throughput between the two.
- **Warm-up pass.** A full `decode<false>` pass runs before timing to warm
  I-cache/D-cache/branch predictors and fault in pages, so the timed runs
  measure steady state, not cold-start.
- **Fresh book per run.** Each of the 5 runs builds a fresh `BookSet`, so a
  run's state doesn't leak into the next.
- **5 runs, median + range — not averaged.** Each run yields a full
  distribution; we take that run's p50/p99/p99.9, then report the **median of
  the 5 per-run values** with the **min–max spread across runs**. Median (not
  mean) is robust to a single noisy run; the spread is the honest disclosure
  of run-to-run jitter. Raw samples are *never* pooled across runs (that would
  hide the variance we're trying to show control of).
- **`do_not_optimize` barrier.** Google-Benchmark-style asm barrier
  (`asm volatile("" : : "r,m"(v) : "memory")`) keeps the decode result live
  so the optimizer can't delete the work — without `volatile`'s memory-store
  cost.

---

## 3. Sample size (why the percentiles are meaningful)

- Fixture: `itch_500m.bin`, a 500 MB slice of real Nasdaq TotalView-ITCH 5.0.
- Multi-symbol watchlist (SPY, QQQ, MSFT, AAPL, TSLA, GOOGL, AMZN) so enough
  book-affecting messages are *applied* to populate the tail — a single
  symbol doesn't give enough applied events for a trustworthy p99.9.
- ~862k applied messages per run. p99.9 needs ~1k samples in the top 0.1%;
  862k gives ~862 there — enough to state p99.9 with a straight face, not
  enough to over-claim p99.99 off one run.

---

## 4. Measurement environment

Develop on macOS (Apple Silicon), **measure on Linux/x86_64**. The Mac's
`cntvct_el0` ticks at 24 MHz (~42 ns/tick) — too coarse to resolve
per-message latency, so sub-42 ns work floors to 0. The real percentiles
require the x86 TSC (GHz resolution). Hence the VM.

### The VM (measured run)
- **Provider / instance:** DigitalOcean CPU-Optimized, 2 vCPU / 4 GB.
- **CPU:** Intel Xeon Platinum 8168 @ 2.70 GHz (Skylake-SP).
- **OS / kernel:** Ubuntu 24.04 LTS, 6.8.0-124-generic.
- **Virtualization:** KVM guest (`pc-i440fx`) — **not bare metal**. Stated
  honestly; a hypervisor sits under this.

### TSC / timing caveats (the interview-honest part)
- `constant_tsc` **present** → TSC ticks at a fixed rate regardless of core
  frequency, so ticks→ns is stable. This is the flag that governs
  measurement validity, and we have it.
- `nonstop_tsc` **absent** → TSC could stop in deep idle. **Does not affect
  this benchmark:** the timed loop is CPU-bound on a busy pinned core that
  never idles, so the "stops in idle" case never triggers. Absence is a
  hypervisor artifact of the KVM guest, not a measurement error.
- **No `cpufreq` in the guest** → can't set a `performance` governor;
  frequency is hypervisor-controlled. `constant_tsc` + pinned busy core give
  stable timing regardless. Governor step is N/A here (would apply on bare
  metal).
- **Core pinning:** `taskset -c 1` pins the benchmark to core 1; the OS lives
  on core 0. Single-threaded workload, so one dedicated core is the right
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

## 5. Results

Raw run captured in `vm_results_before.txt` (repo root). This is the
**"before"** state — the p99.9 tail is an unfixed measurement artifact
(see §6). Numbers below are stable across 3 separate invocations of each
binary; the range column is the in-harness spread over 5 runs.

Calibrated TSC: **2.694 GHz** — matches the 2.70 GHz Xeon, so ticks→ns is
correct and the numbers are valid.

### Latency — per-message dispatch + apply (`bench_latency`, taskset -c 1)
| Percentile | Median of 5 runs | Range across runs | Across 3 invocations |
|---|---|---|---|
| p50 | ~120 ns | 117–124 ns | 118.8 / 121.0 / 121.7 |
| p99 | ~427 ns | 418–439 ns | 423.8 / 428.3 / 432.7 |
| p99.9 | ~16 µs | 15.9–16.2 µs | 15931 / 15981 / 16031 ns |

### Throughput (`bench_feed`, taskset -c 1, min of 20 iters)
- **~14.0 ns/msg, ~71.4 M msgs/sec** (13.90–14.06 ns/msg across runs).

> **Why throughput (14 ns) < latency p50 (120 ns) — not a contradiction.**
> `bench_feed` times *all* framed messages; ~99% are non-watchlist symbols
> that hit `books.get(locate) → nullptr → return` (a cheap early-out),
> amortizing the average down. `bench_latency` times *only* the ~862k
> **applied** watchlist messages that actually mutate a book — the real
> per-op cost. Different denominators, both honest.

### `perf stat` (hardware counters, `bench_feed`)
```
instructions     23_900_443_933
cache-references    253_694_291
cache-misses        206_030_644   # 81.21% of all cache refs
branches          4_560_763_826
branch-misses        45_071_727   # 0.99% of all branches
```
- **`cycles` read as 0** on this KVM guest — the cycle PMU is gated even
  though the instruction counter works. So **IPC is not claimed** (it needs
  valid cycles). Honest limitation of the environment.
- **81% cache-miss looks alarming but is expected:** `bench_feed` streams a
  500 MB buffer once, linearly, with no reuse — nearly all misses are
  *compulsory* (cold), not a locality bug. The hardware prefetcher hides the
  latency (throughput stays at 71 M/s). Branch prediction is healthy (0.99%).

### `perf record` — hot path / tail attribution (`bench_latency`, frame-pointer)
Children view (who owns total time):
```
main                              73.6%
  add_order                       19.8%   <- real book-mutation hot path
  on_directory -> OrderBook ctor   5.5%
    asm_exc_page_fault             3.2%   <- THE TAIL: first-touch faults
      handle_mm_fault -> do_anonymous_page -> __alloc_pages -> clear_page
  istream::read                    5.0%   <- fixture load, OFF the timed clock
```
Self view (where steady-state cycles go):
```
main                  38.1%   (decode loop + dispatch, inlined)
add_order             19.5%   (book mutation)
std::__introsort_loop  6.8%   (std::sort of samples — bench bookkeeping,
                               outside the timed region, not engine cost)
```
**Note on reading this profile — recenter is invisible here but IS the tail.**
`recenter()` is called from inside `add_order` and is inlined under LTO, so its
cost is attributed to `add_order`/`main` rather than appearing as its own frame.
The page-fault chain under `OrderBook::OrderBook` is real but is **not** the
histogram tail: the `R` (directory) branch of `decode_message` returns *before*
the `if constexpr (Timing)` block, so book construction is never sampled into
`LatencySink`. See §6 — the tail was isolated by direct instrumentation, not by
this profile.

---

## 6. Interpreting the tail (interview story)

The p50 is the common case: dispatch + a slot mutation in the preallocated
pool + a touch-bitmap update — ~120 ns.

**The p99.9 tail is `recenter()` — specifically its `rebuild_bitmap()` scan.**
Isolated by direct instrumentation, not by the profile (recenter inlines into
`add_order` under LTO, so it has no frame of its own in `perf report`).

### The evidence
Counting samples ≥ 5 µs against the book's own `recenters_` counter, per run:

| | run 0 | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|---|
| samples ≥ 5 µs | 2243 | 2162 | 2213 | 2155 | 2146 |
| total recenters | — | — | — | — | **2124** |

**~1 tail sample per recenter, within 1–4%.** Per-symbol on the same run:

```
AAPL   recenters=154   far=381    not_found=1160  evicted=3244
AMZN   recenters=189   far=4394   not_found=351   evicted=601
GOOGL  recenters=173   far=1692   not_found=2153  evicted=1757
MSFT   recenters=2     far=4870   not_found=8     evicted=1
QQQ    recenters=202   far=148    not_found=2252  evicted=3620
SPY    recenters=399   far=306    not_found=2127  evicted=2350
TSLA   recenters=1005  far=6303   not_found=5828  evicted=5872
TOTAL  recenters=2124
```

Two independent facts rule out the page-fault hypothesis:
1. **The tail reproduces on macOS/arm64** (~8 µs, 2124 recenters) — a different
   kernel, allocator, and page-fault path. A first-touch-fault explanation does
   not survive an OS change; a fixed-cost scan does.
2. **The tail is tight** (15.9–16.1 µs on x86, 8.0–8.4 µs on arm64). Page faults
   jitter; `rebuild_bitmap()` is a constant 2 × 4096-level sequential scan
   (~192 KB), which is exactly the shape of a fixed-cost operation. The ~2×
   difference between platforms tracks memory bandwidth, not fault cost.

### Root cause (a correctness bug, not just latency)
`add_order` calls `recenter(price)` whenever an add lands outside the ±2048-tick
window but within ±4096 of centre — i.e. it re-centres the entire book **onto a
single incoming order**. One deep resting order $25 from mid drags the window to
it, **evicting the near-mid book**; the next normal add drags it back. Ping-pong,
~2100× per pass.

The `evicted` / `not_found` columns are the damage: ~14 k resting orders evicted
per pass, and ~14 k subsequent delete/execute/cancel messages then fail to find
their order. Contrast **MSFT: 2 recenters → 1 evicted → 8 not_found** — its price
level puts outliers beyond the recenter band, so they take the `is_far` path and
the book stays intact. That is the correct behaviour, reached by accident.

**So the latency benchmark surfaced a book-state-corruption bug.** That is the
more valuable finding.

### The fix (pending)
Recenter on **touch drift** — when best bid/ask approaches the window edge —
never on a single outlier add. Outliers in the band belong on the existing
`is_far` path (which MSFT demonstrates works). Expected result: recenters drop
by an order of magnitude, the p99.9 tail collapses toward p99, and the
evicted/not_found counts go to near-zero.

### Interview framing
*"p50 120 ns, p99 427 ns, p99.9 16 µs. perf pointed at page faults in the book
constructor — but that path returns before the timing block, so it couldn't be
the tail. I instrumented the recenter counter instead: 2124 recenters vs ~2150
samples over 5 µs, 1:1. Then I re-ran on a different OS and the tail reproduced
at the same message count, which killed the page-fault theory outright. The real
cause was a recenter policy that re-centres on a single outlier order — and the
evicted/not_found counters showed it was silently dropping resting orders. So
the latency tail was a symptom of a correctness bug."*

> **Two corrections on record.** (1) An early guess blamed recentering with no
> evidence. (2) `perf` then pointed at constructor page faults, which I wrote up
> as settled — wrong, because that code path is never timed. Only direct
> counter instrumentation plus a cross-OS reproduction settled it. Both errors
> are kept here deliberately: the profile was *misleading*, and the discipline
> that caught it was checking whether the proposed cause was even inside the
> measured region.

---

## 7. What this run does NOT claim

- **Not bare metal** — KVM guest; a real desk measures on tuned bare metal
  with isolcpus, hugepages, and NIC kernel-bypass. This is the honest
  ceiling of a $0.03 cloud run.
- **No p99.99** off a single day's slice — sample count supports p99.9, not
  a stable p99.99.
- **Replay, not live** — no NIC receive, no wire-to-book end-to-end. The
  number is the *engine's own* dispatch+apply cost, which is exactly the
  bounded, defensible thing to put a number on.
