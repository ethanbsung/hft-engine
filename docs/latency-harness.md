# Module: Latency Harness

**File(s):** `include/hft/latency.hpp` (to create); builds on `platform::now_ns()`
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
Measure the system's own latency **honestly**. Timestamp taps between
stages, aggregate into distributions (not just averages), and report.
This is what lets you say true things about performance — and per the
README rule, you may not claim a latency number you haven't measured.

## What to measure
- **Per-stage taps** on the hot path: recv → parsed → book-updated →
  strategy-decided → order-built → (sim) fill. Store `nanos_t` timestamps
  and diff adjacent stages.
- **End-to-end:** recv_ts → order-out.
- **The plumbing:** ring buffer push/pop (see ring-buffer.md benchmark).

### Your-code latency = later stage − `recv_ts` (the number that's yours)
The most important, most resume-relevant number is **your own pipeline's
latency**, independent of the exchange and network:
`now_ns()` at a later stage **minus `recv_ts`**. `recv_ts` is stamped in
the socket reader (see feed-handler.md), so `(after-strategy) − recv_ts`
is pure own-code time. Report this prominently.

### NEVER cross-subtract `exchange_ts` and `recv_ts`
There are three timestamps measuring three different things:

| Timestamp | Taken where | Measures |
|---|---|---|
| `exchange_ts` | parsed from the message `event_time` | exchange → you (network + exchange; the ms part — *not* your latency) |
| `recv_ts` | socket reader, right after `ws.read()` | entry point of your system |
| stage taps | after parse / book / strategy … | **your own code**, stage by stage |

`recv_ts` and stage taps share **one monotonic clock**
(`platform::now_ns()`) — subtracting them is valid. **Do not** compute
`recv_ts − exchange_ts` as a "network latency": `exchange_ts` is the
*exchange's wall clock*, `recv_ts` is *your monotonic clock* — different,
unsynchronized clocks. The difference is meaningless and can even go
negative from clock skew. Honest exchange→you timing needs clock sync
(PTP/NTP) and NIC hardware timestamping you won't have here, so treat any
cross-clock number as unreliable-by-construction and report **own-clock
stage deltas** instead.

**Under ITCH replay this is even more clear-cut.** `exchange_ts` is ns
since midnight ET **on a trading day in 2020**, and `recv_ts` is stamped
when *your simulator's* UDP packet arrives. Their difference is years, not
microseconds. There is no exchange→you latency to measure at all: the
number that is *yours* — decode → book → strategy, own-clock — is the only
number, and now it's the whole number (ARCHITECTURE §2).

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
