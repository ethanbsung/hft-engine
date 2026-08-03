# Module: Latency Harness

**File(s):** `include/hft/latency_sink.hpp`; percentile reporting in
`bench/bench_latency.cpp`; builds on `platform::now_ns()` / TSC reads
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
recv → parsed → book-updated → strategy-decided → order-built → fill — storing
`nanos_t` timestamps and diffing adjacent stages, plus an end-to-end
`recv_ts → order-out`, plus ring-buffer push/pop cost
(design/ring-buffer.md). None of those stages exist yet.

### Own-code latency = later stage − `recv_ts`
The number that belongs to this engine is its own pipeline latency, independent
of exchange and network: `now_ns()` at a later stage minus `recv_ts`, where
`recv_ts` is stamped by the socket reader before decode
(design/feed-transport.md §4).

### Never cross-subtract `exchange_ts` and `recv_ts`
Three timestamps measure three different things:

| Timestamp | Taken where | Measures |
|---|---|---|
| `exchange_ts` | parsed from the ITCH message header | Nasdaq's own clock at publication — not this engine's latency |
| `recv_ts` | socket reader, immediately after the packet arrives | entry point of this system |
| stage taps | after parse / book / strategy … | this engine's code, stage by stage |

`recv_ts` and the stage taps share one monotonic clock
(`platform::now_ns()`), so subtracting them is valid. `recv_ts − exchange_ts`
is **not** a network latency: `exchange_ts` is the exchange's wall clock and
`recv_ts` is a local monotonic clock — unsynchronized, and the difference can
even go negative from skew. Genuine exchange→engine timing needs PTP sync and
NIC hardware timestamping, neither of which is available here.

Under replay the point is sharper still: `exchange_ts` is ns since midnight ET
on a trading day in 2020, so the difference is measured in years. There is no
exchange→engine latency to recover, which is why the only number reported is
own-clock.

## Report distributions, not averages
HFT cares about **tails**. Report **min / p50 / p90 / p99 / p99.9 / max**,
not mean. A good p50 with a bad p99 is a bad system. Use an HDR-histogram-
style bucketed histogram so recording a sample is O(1) and allocation-free.

## Design notes
- **Recording must be cheap and non-perturbing.** Reading the clock has
  overhead; measure that overhead itself and subtract/annotate it.
  `platform::now_ns()` (steady_clock) is the portable start; a calibrated
  TSC reader is the documented later upgrade (already noted in
  `platform.hpp`). Swap it behind the same signature — don't change taps.
- **Never format/print on the hot path.** Record raw samples into a
  preallocated histogram (or push to the telemetry ring); aggregate and
  print on the cold/logger thread.
- **Measure on Linux, pinned.** Mac numbers are not meaningful (README).
  Stamp every report with `platform::platform_tag()` so a Mac number is
  never mistaken for a Linux one.

## Interface (contract sketch)
```cpp
class LatencyHistogram {         // one per tap / metric
public:
    void record(nanos_t sample) noexcept;   // O(1), no alloc
    // percentile(p), min, max, count — read off the cold path
};
```

## Latency notes
The harness itself must be near-zero overhead or it distorts what it
measures. Benchmark `record()`; keep it a bucket index + increment.

## Done checklist
- [ ] O(1), allocation-free `record()` (bucketed histogram)
- [ ] Percentiles: p50/p90/p99/p99.9 + min/max/count
- [ ] Clock-read overhead measured and reported
- [ ] Per-stage taps wired on the hot path
- [ ] Reports stamped with platform_tag; aggregated off hot path
- [ ] Bench `record()` overhead (Linux, pinned)
