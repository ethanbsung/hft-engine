# Benchmarks & Profiling — Results and Methodology

The numbers this project is allowed to claim, and *exactly* how they were
produced. No latency number is stated anywhere in this project that was not
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

### The VM (measured runs)
- **Provider / instance:** DigitalOcean CPU-Optimized, 2 vCPU / 4 GB.
- **CPU:** Intel Xeon Platinum **8280** @ 2.70 GHz (Cascade Lake) for the
  current results. The earlier pre-fix run landed on a Xeon **8168**
  (Skylake-SP) — same nameplate clock, different microarchitecture. See the
  caveat in §6 before comparing throughput across the two.
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

Raw output in `vm_results_after.txt` (repo root); the superseded pre-fix run is
kept in `vm_results_before.txt` for the comparison in §6.

**Environment:** Intel Xeon Platinum 8280 @ 2.70 GHz (Cascade Lake),
DigitalOcean CPU-Optimized 2 vCPU / 4 GB, Ubuntu 24.04 / 6.8.0-124-generic, KVM
guest. Calibrated TSC **2.694–2.695 GHz**, matching the nameplate clock — so
ticks→ns is correct.

**Sample base:** 862 057 applied messages per run, across 7 symbols.
**Runs:** 4 invocations × 5 runs = **20 independent measurements**.

### Latency — per-message dispatch + apply (`bench_latency`, `taskset -c 1`)

| Percentile | Median | Across all 20 runs |
|---|---|---|
| **p50** | **123 ns** | 120.24 – 126.18 |
| **p99** | **426 ns** | 423.08 – 431.26 |
| **p99.9** | **590 ns** | 577.47 – 601.98 |

**Instrumentation overhead: 34–36 cycles (12.62 / 13.36 ns).** Eight samples,
all exactly one of those two values — the empty fenced bracket is deterministic,
not noisy. This is a floor included in every percentile above, so the engine's
own dispatch+apply cost is **≈110 ns at p50**. Percentiles are reported raw;
see §2 for why the overhead is disclosed rather than subtracted.

### Throughput (`bench_feed`, `taskset -c 1`, min of 20 iters)
**13.3 ns/msg, 74.8 M msgs/sec** (13.17 – 13.39 ns/msg across 4 runs).

> **Why throughput (13 ns) < latency p50 (123 ns) — not a contradiction.**
> `bench_feed` times *all* 16.3 M framed messages; ~99% are non-watchlist
> symbols that hit `books.get(locate) → nullptr → return` (a cheap early-out),
> amortising the average down. `bench_latency` times *only* the 862 k
> **applied** messages that actually mutate a book — the real per-op cost.
> Different denominators, both honest. **Quote the p50, not the throughput**:
> the throughput figure mostly measures the framing loop.

### Book health at end of replay (same runs)
`not_found = 0` for all 7 symbols, all books uncrossed. Far orders (ITCH's
\$199,999.99 sentinel, penny stubs, genuinely deep passive orders) total 1 368,
dominated by TSLA's 821. Correctness and performance measured on the same
replay, so the numbers describe a book that is actually right.

> **Why throughput (14 ns) < latency p50 (120 ns) — not a contradiction.**
> `bench_feed` times *all* framed messages; ~99% are non-watchlist symbols
> that hit `books.get(locate) → nullptr → return` (a cheap early-out),
> amortizing the average down. `bench_latency` times *only* the ~862k
> **applied** watchlist messages that actually mutate a book — the real
> per-op cost. Different denominators, both honest.

### `perf stat` (hardware counters, `bench_feed`)
```
instructions     17_817_911_865
cache-references    282_157_815
cache-misses        206_164_332   # 73.07% of all cache refs
branches          3_488_380_602
branch-misses        42_304_746   # 1.21% of all branches
```
Instruction count fell 25% versus the pre-fix run (23.9 B → 17.8 B) and the
cache-miss rate 8 points (81.2% → 73.1%), both from removing the recentre path
and its O(window) bitmap rebuilds.

A second `perf stat` on `bench_latency` is in the raw output but is **less
useful for judging the engine**: that binary also runs the 200 k-iteration
overhead loop and five `std::sort`s of 862 k samples, which is why its
branch-miss rate is higher (2.17%). Quote `bench_feed`'s counters when
discussing the decode path.
- **`cycles` read as 0** on this KVM guest — the cycle PMU is gated even
  though the instruction counter works. So **IPC is not claimed** (it needs
  valid cycles). Honest limitation of the environment.
- **81% cache-miss looks alarming but is expected:** `bench_feed` streams a
  500 MB buffer once, linearly, with no reuse — nearly all misses are
  *compulsory* (cold), not a locality bug. The hardware prefetcher hides the
  latency (throughput stays at 71 M/s). Branch prediction is healthy (0.99%).

### `perf record` — where time goes (`bench_latency`, frame-pointer unwinding)
Children view:
```
main                             60.7%
  on_directory -> OrderBook ctor  6.3%
    asm_exc_page_fault            4.3%   <- startup arena commit, NOT in the
      do_anonymous_page -> clear_page_erms      latency histogram (see below)
  istream::read                   5.0%   <- fixture load, off the timed clock
```
Self view (where steady-state cycles go):
```
main                  43.9%   (decode loop + dispatch, inlined)
add_order             13.2%   (book mutation)
std::__introsort_loop  7.8%   (std::sort of samples — bench bookkeeping,
                               outside the timed region, not engine cost)
```
`add_order` self time fell from 19.5% pre-fix to 13.2%, which is the recentre
path leaving the hot function.

**The page-fault chain is real but is not in the histogram.** It is the kernel
committing each book's ~4 MB of arrays on first touch, inside the `OrderBook`
constructor, which runs from the `R` (directory) branch of `decode_message` —
and that branch returns *before* the `if constexpr (Timing)` block. It is
startup cost, correctly excluded from per-message numbers, and not something to
"fix". An earlier version of this document mistook it for the tail; see §6c.
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

### The fix (applied)
Replaced the sliding window with a **fixed** one: `base_price_` is set at
construction from a per-symbol reference price (hardcoded for this replay; in
production, previous close) and never moves. The window widened from 4 096 to
32 768 ticks (±\$163.84), which required extending the touch bitmap from two
tiers to three. Out-of-window orders take the existing `is_far` path.
`recenter()`, `rebuild_bitmap()` and `evict_level()` are retired.

Measured effect on Linux/x86_64 (same fixture, same harness):

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

p50 and p99 are unchanged, as expected — the fix removed a rare expensive path,
it did not alter the common-path instruction sequence. The tail is where a
window move showed up, and that is what collapsed.

> **Caveat on the throughput comparison.** DigitalOcean provisioned a **Xeon
> 8168** for the pre-fix run and a **Xeon 8280** for the post-fix run — same
> 2.70 GHz nameplate, different microarchitecture (Skylake-SP vs Cascade Lake).
> Part of the ~5% throughput gain may be the newer silicon rather than the fix,
> so it should not be claimed outright. The instruction-count drop (25%) is
> hardware-independent and *is* attributable. The 27× tail reduction is far too
> large for any CPU difference to explain.

The residual ~225 samples ≥ 5 µs per run (0.026% of 862 k) are OS scheduling
noise, not engine cost. Confirmation: running the same binary under
`perf record` raised that count to ~730/run — the profiler's own sampling
interrupts — while p50 and p99 barely moved. Interference, not the code.

---

## 6b. A second bug the same sanity-checking found: `RefIndex::erase`

After the window fix, a full-fixture replay was checked for book consistency
before publishing numbers. **6 of 7 books ended crossed** — best bid above best
ask, which is impossible in a real book. TSLA showed bid \$644.00 / ask \$624.50.

### How it was isolated
1. **Ruled out the data.** An independent Python scan of the raw ITCH bytes
   traced every SPY order's lifecycle: 113 073 adds, 109 898 deletes, and
   **zero messages referencing an unknown ref**. The feed is self-consistent, so
   the book was losing orders on its own. (The fixture also starts at true
   session start — message 0 is `S`/`O` at 03:02 ET — ruling out a mid-session
   join.)
2. **Ruled out capacity.** Peak simultaneously-resting SPY orders: 3 177 against
   a 65 536 pool. 5% utilisation.
3. **Differential fuzz** of `RefIndex` against `std::unordered_map` failed after
   **4 operations with 2 live entries** — small enough to reduce by hand.
4. **Minimal case:** three refs colliding into one bucket; erase the first, and
   the *third* becomes unreachable.

### Root cause
`erase` computed shift distances via a lambda capturing `hole` by reference
while reassigning `hole` inside the loop, so after the first shift the
comparison used an inconsistent origin and skipped entries that needed
relocating. `find` stops at the first empty slot, hit the stale hole, and gave
up — orphaning everything beyond it. It needs a probe chain of length ≥ 3 to
manifest, which is why hand-written unit tests missed it.

### Effect
Orphaned refs meant delete/execute messages could not resolve, so those orders
rested forever and the book crossed as the market moved away.

| | before | after |
|---|---|---|
| books crossed | **6 of 7** | **0** |
| not_found (7 symbols) | 543 | **0** |
| SPY | bid 32480 / ask 32423 (crossed) | bid 32438 / ask 32440 (\$0.02) |
| TSLA | bid 64400 / ask 62450 (crossed \$19.50) | bid 62408 / ask 62450 (\$0.42) |

`not_found == 0` now matches the Python ground truth exactly. Regression
coverage: 6 collision-chain tests plus a differential fuzz verifying every live
entry after each of 20 000 operations.

---

## 6c. Methodology notes (the part worth reading twice)

**`perf` pointed at the wrong thing, twice.** Its call graph showed a page-fault
chain under `OrderBook::OrderBook`, which looked like a conclusive explanation
for the tail. It was not: the `R` (directory) branch of `decode_message` returns
*before* the `if constexpr (Timing)` block, so book construction is never
sampled into the histogram. The faults were real and irrelevant.

What actually settled each question:
- **Checking whether the proposed cause was inside the measured region.** This
  alone eliminated the page-fault theory.
- **Direct counter instrumentation.** 2 124 recentres vs ~2 150 samples over
  5 µs — a 1:1 match no profile could have shown, because `recenter` inlines
  into `add_order` under LTO and has no frame of its own.
- **Cross-platform reproduction.** The tail reproduced on macOS/arm64 at the
  same message count and with the same tightness. A first-touch-fault
  explanation cannot survive a different kernel and allocator; a fixed-cost scan
  can.
- **A second, independent implementation.** The Python scan over raw bytes gave
  ground truth the C++ could be checked against, and is what proved the book —
  not the data — was at fault.

**Corrections on record.** An early version of this document asserted the tail
was constructor page faults, and stated it as settled. That was wrong. It is
left documented rather than quietly edited out, because the failure mode —
trusting a profile without checking whether the hot code it names is inside the
timed region — is more instructive than the eventual answer.

---

## 7. What this run does NOT claim

- **Not bare metal** — KVM guest; a real desk measures on tuned bare metal
  with isolcpus, hugepages, and NIC kernel-bypass. This is the honest
  ceiling of a $0.03 cloud run.
- **No IPC** — the `cycles` PMU read as 0 on this guest (instruction counting
  works, cycle counting is gated), so instructions-per-cycle cannot be computed
  from this data.
- **No p99.99** off a single day's slice — sample count supports p99.9, not
  a stable p99.99.
- **Replay, not live** — no NIC receive, no wire-to-book end-to-end. The
  number is the *engine's own* dispatch+apply cost, which is exactly the
  bounded, defensible thing to put a number on.
- **Pre-market data** — the fixture spans 04:00–09:30 ET. Books are thinner and
  quieter than during regular hours, so message rates and book depth are not
  representative of the full session.

---

## 8. How to state this honestly

The defensible one-line claim:

> **p50 123 ns / p99 426 ns per-message dispatch + apply**, over 862 k applied
> Nasdaq TotalView-ITCH 5.0 messages across 7 symbols, on a pinned x86 core at
> 2.694 GHz. Instrumentation overhead (34–36 cycles) measured and included;
> net ≈110 ns.

Calibration, so this is neither oversold nor undersold:

- **There is no public number to rank this against, and claiming one is a
  trap.** Firms do not publish per-message book-apply latency; it is
  competitive information. The figures that circulate publicly —
  sub-microsecond, commonly 1–5 µs — are almost always **tick-to-trade**
  (NIC in → decision → NIC out), which includes network stack, decode, book,
  strategy, risk and order encoding. Your 123 ns is *one stage inside that
  path*, so it is not comparable to those figures and should never be
  presented as if it were. If asked "how does this compare?", the correct
  answer is that the comparison does not exist, followed by what the number
  *is* a measurement of.
- **What can be said about the magnitude, from first principles:** ~110 ns at
  2.694 GHz is ~300 cycles for a hash probe, a level access, a list unlink and
  a three-tier bitmap update. An L3 hit is ~40 cycles and a DRAM miss ~200–300,
  so that budget is consistent with a couple of cache misses plus real work —
  i.e. the number is *physically plausible for what the code does*, which is a
  claim that can be defended by reasoning rather than by an appeal to
  unpublished benchmarks. The 73% cache-miss rate in §5 supports it.
- **The genuinely fast book implementations are FPGAs**, where decode-and-update
  runs in tens of nanoseconds. That is a hardware comparison, not a software
  one.
- **No optimisation pass has been done.** Everything so far removed a
  pathology; nothing has yet targeted the common path. Say this before someone
  asks.
- **The throughput number is the weaker one.** 74.8 M msgs/sec is dominated by
  the ~99% of messages that early-out on a non-watchlist symbol. Leading with
  it invites a fair objection. Lead with p50.
- **What is actually strong is the methodology**, and that is the part worth
  defending: a distribution rather than an average, 20 runs with the spread
  disclosed, timer overhead quantified rather than hidden, and correctness
  (`not_found = 0`, no crossed books) verified on the same replay that produced
  the timings.
- **The strongest single item is not a number at all** — it is that the
  measurement process found two real bugs (§6, §6b), and that the reasoning
  which isolated them is reproducible and written down.

---

## 9. Open items

1. **An optimisation pass on the common path.** Everything so far removed a
   pathology; nothing has yet made the 110 ns itself faster. `add_order` is 13%
   of self time and the cache-miss rate is 73% — the ref-index probe is the
   obvious first suspect (layout, prefetch, or a cheaper mix). This would turn
   "I fixed a defect" into "I profiled and optimised the hot path", which is a
   stronger claim.
2. **A full-session fixture.** The current one ends at 09:30 ET, so the
   end-of-day invariant (book empty after the closing `S` message) cannot be
   asserted, and all numbers come from pre-market flow.
3. **A staleness signal.** `not_found_` is currently a counter; in a live system
   it would trip a per-symbol health flag that halts quoting and triggers a
   GLIMPSE re-snapshot (ITCH carries no in-feed snapshot). Worth building if the
   engine grows a strategy layer.
4. **Bare-metal comparison.** Everything here is a KVM guest. A tuned bare-metal
   run with isolcpus and hugepages would show how much of the tail is the
   hypervisor.
