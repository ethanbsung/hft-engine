# Module: Ring Buffer (SPSC, lock-free)

**File(s):** `include/hft/ring_buffer.hpp` (to create)
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
A bounded, **single-producer single-consumer**, wait-free/lock-free queue
to hand messages between threads **without locks and without allocation**.
Used at the **hot↔cold** and **I/O↔hot** boundaries (ARCHITECTURE §3) —
*not* between hot stages.

This is the flagship lock-free learning exercise. Build it once, correctly,
and understand every line.

## Where it's used
- Socket reader → hot thread (raw frames)
- Hot thread → order sender (outbound orders)
- Hot thread → logger/telemetry (latency samples, events)
- Feed handler → snapshot/recovery thread (gap signals)

All of these are exactly one producer, one consumer → **SPSC** is
sufficient. Don't build MPMC; it's slower and you don't need it.

## Design points to get right (the whole reason to build it)
- **Bounded, power-of-two capacity** so index wrap is a mask (`& (N-1)`),
  not a modulo.
- **Preallocated slot storage** — no `new`/`delete` after construction.
- **`head` and `tail` are `std::atomic`**, but each is written by *only
  one* side (producer owns tail, consumer owns head). This is what makes
  SPSC cheap.
- **Memory ordering:** the producer `store`s tail with `release` after
  writing the slot; the consumer `load`s tail with `acquire` before
  reading the slot. `head`/`tail` reads of *your own* index can be
  `relaxed`. Understand *why* — this is the core `<atomic>` lesson.
- **Cache-line padding / `alignas(64)`:** `head` and `tail` must be on
  **separate cache lines**. If they share a line, the producer and
  consumer cores ping-pong that line back and forth — research showed a
  **3.1× throughput collapse** from exactly this false-sharing bug.
- **Shadow indices (optimization):** cache the other side's index locally
  and only re-read the shared atomic when your local copy says the queue
  looks full/empty. Reduces cross-core reads. Add *after* correctness.

## Interface (contract sketch)
```cpp
template <typename T, size_t Capacity>   // Capacity power of two
class SpscRing {
public:
    bool try_push(const T& item) noexcept;   // false if full
    bool try_pop(T& out) noexcept;           // false if empty
    // (optionally: emplace, size_approx)
};
```
Return `bool` rather than block — the caller decides whether to busy-spin
or drop.

## Latency notes
Target: single-digit-to-tens of ns per push/pop once padded correctly.
**Measure this on Linux, pinned** (`bench/`), and *do not* write
"lock-free"/"wait-free" in a comment until the benchmark backs it up
(README rule).

## Done checklist
- [ ] Power-of-two capacity, mask-based wrap
- [ ] Preallocated storage, no hot-path allocation
- [ ] Correct acquire/release ordering (documented per line)
- [ ] `head`/`tail` on separate cache lines (`alignas`)
- [ ] Single-thread correctness test (fill/drain/wrap)
- [ ] Two-thread stress test (producer+consumer, verify no lost/dup items)
- [ ] Benchmark (Linux, pinned) — record ns/op before claiming anything
- [ ] (Later) shadow-index optimization + re-benchmark
