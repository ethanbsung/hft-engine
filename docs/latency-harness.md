# Module: Latency Harness

**File(s):** `include/hft/latency_sink.hpp`; percentile reporting in
`bench/bench_latency.cpp`; times with `platform::read_cycles()`, converted via
`cycles_per_sec()` / `cycles_to_ns()`
**Phase:** 1 · **Status:** 🚧 partial — a compile-time-gated (`template<bool
Timing>`) tap around dispatch+apply plus p50/p99/p99.9 reporting are working
and produced the numbers in `docs/benchmarks.md`. The staged multi-tap design
below (per-stage timestamps through the pipeline) is **not** built; there is
one tap, not a chain.

## Responsibility
Measure the engine's own latency: timestamp the hot path, aggregate into
distributions rather than averages, and report. Results and full methodology
are in `benchmarks.md`.

## What exists today
One tap, around dispatch + apply — the region `benchmarks.md` reports on. It is
gated at compile time on `template<bool Timing>`, so the untimed instantiation
carries no counter reads at all, and writes raw cycle deltas into a
`LatencySink` (a `std::vector<uint32_t>`, reserved up front by the benchmark so
`record` does not allocate mid-run). Percentiles are computed off the hot path,
after the run, in `bench/bench_latency.cpp`.

## The staged design (not built)
Once there is a pipeline to stage, the intended shape is per-stage taps —
recv → parsed → book-updated → strategy-decided → order-built → fill —
diffing adjacent stages, plus an end-to-end `recv_ts → order-out`, plus
ring-buffer push/pop cost (design/ring-buffer.md). None of those stages exist
yet. Note the built tap stores a **delta per sample**, not a timestamp per
stage; a chain of stages needs the timestamps themselves retained, so
`LatencySink` grows a wider sample type when that lands.

### Own-code latency = later stage − entry
The number that belongs to this engine is its own pipeline latency, measured
entirely on one clock: a later stage tap minus an entry tap. Today that is the
single dispatch+apply bracket; under the staged design the entry point becomes
`recv_ts`, stamped by the socket reader before decode
(design/feed-transport.md §4). Every tap must read the same clock for the
differences to be valid subtractions — today that is `read_cycles()`
throughout.

The ITCH message header carries an exchange timestamp (ns since midnight ET),
but **this engine never parses it** — the decoder skips that field, and no
reported number is derived from it. That is deliberate: it is the exchange's
wall clock, unsynchronized with the local monotonic clock, so differencing the
two yields nothing meaningful. Genuine exchange→engine timing needs PTP sync
and NIC hardware timestamping, neither of which is available here — and under
replay the capture is from a trading day years in the past, so there is no
exchange→engine latency to recover in the first place. Own-clock is the only
number reported.

## Report distributions, not averages
HFT cares about **tails**. A good p50 with a bad p99 is a bad system, so the
report is percentiles, never a mean.

**Reported today** (`bench_latency`): **p50 / p99 / p99.9**, each as the median
of 5 runs with the min–max range across those runs, plus the measured
instrumentation overhead. Run-to-run range is the stability signal; sample min
and max are not reported, since the extremes of a replay are dominated by
one-off effects (first-touch page faults, scheduler noise) rather than the
code under test.

Percentiles come from sorting the sample vector after the run and indexing
directly (`samples[sz * 99 / 100]`) — O(n log n), but entirely off the hot
path, so it costs the measurement nothing. A bucketed HDR-style histogram is
the upgrade if `record()` ever needs to run in production rather than in a
benchmark; it is not needed while sampling happens under a `Timing` template
flag that compiles out.

## Design notes
- **Recording must be cheap and non-perturbing.** Reading the clock has
  overhead, so it is measured rather than assumed. The TSC upgrade that
  `platform.hpp` still describes as a TODO has **already happened** for this
  harness: timing reads `read_cycles()` (`lfence; rdtsc; lfence` on x86_64,
  `cntvct_el0` on arm64), not `now_ns()`. `now_ns()` remains the engine's
  general-purpose clock and is used inside `cycles_per_sec()` to calibrate
  the TSC against it.
- **Never format/print on the hot path.** The timed region does one
  `push_back` of a raw cycle delta; sorting, tick→ns conversion, and all
  printing happen after the run. There is no telemetry ring yet — nothing to
  push to — so "off the hot path" currently means "after the run", not "on a
  logger thread".
- **Measure on Linux, pinned.** Mac numbers are not meaningful (README).
  Every report is stamped with `platform::platform_tag()` so a Mac number is
  never mistaken for a Linux one. Pinning is **external** — `taskset -c 1` on
  the command line (see `build.sh`), not `pin_thread_to_core()`, which is
  declared in `platform.hpp` but not called by any benchmark.

## Interface
`include/hft/latency_sink.hpp`, in full:

```cpp
struct LatencySink {
    std::vector<uint32_t> samples;
    void record(uint64_t delta) {
        samples.push_back(static_cast<uint32_t>(delta));
    }
};
```

Deliberately minimal. `record` stores a raw **cycle delta**, not nanoseconds —
the tick→ns conversion happens once per percentile after the run, so the hot
path never touches a floating-point divide. `uint32_t` halves the sample
footprint and still covers ~1.6 s of ticks at 2.7 GHz; deltas that large mean
the run was descheduled, not that the code was slow. The caller
(`bench_latency.cpp`) `reserve()`s the full sample count up front, which is
what makes `push_back` allocation-free in the timed region — the sink does not
enforce that itself.

The tap that feeds it, in `decode<Timing>`:

```cpp
if constexpr (Timing) {
    uint64_t t0 = platform::read_cycles();
    apply();
    uint64_t t1 = platform::read_cycles();
    sink->record(t1 - t0);
} else {
    apply();
}
```

The dispatch `switch` lives in the `apply()` lambda, so both instantiations run
identical work and the timed one only adds the two fenced reads around it.
`decode<false>` emits no counter read and no branch — the `if constexpr`
discards that arm at compile time, which is why `sink` may be null there.

## Latency notes
The harness itself must be near-zero overhead or it distorts what it measures.
`bench_latency` quantifies this rather than assuming it: an empty fenced
bracket, min over 200k reads, reported alongside the percentiles as
`instrumentation overhead` — 34–36 cycles (12.6–13.4 ns) on the reference box.
It is **included** in every reported percentile, never subtracted out, so the
p50 in `benchmarks.md` is the honest measured cost of tap + work together.

## Done checklist
- [x] Allocation-free `record()` in the timed region (caller reserves)
- [x] Percentiles: p50/p99/p99.9, median + range over 5 runs
- [x] Clock-read overhead measured and reported
- [x] Reports stamped with `platform_tag`; percentiles computed after the run
- [ ] Per-stage taps wired on the hot path (needs a pipeline to stage)
- [ ] Bucketed histogram — only if `record()` moves into production
