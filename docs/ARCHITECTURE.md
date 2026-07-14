# hft_core — System Architecture

> The reference you come back to before implementing each module.
> Decisions here are deliberate and explained; when a decision changes,
> change it *here* first, then in code.

This document is the source of truth for the **overall shape** of the
system. Each module has its own file in this directory
(`docs/<module>.md`) with its responsibility, interface, data
structures, latency notes, and a "done" checklist. Start here, then
open the module file when you sit down to build that module.

---

## 1. What this system is (and isn't)

**Is:** a from-scratch, production-*shaped* HFT engine built as a
learning vehicle. Every component is built the way a real low-latency
trading desk would build it — real order book semantics, real feed
normalization, real latency measurement, real risk checks — at a scale
one person can actually complete and run.

**Isn't:** a co-located, kernel-bypass, FPGA, microsecond-race system.
Not because those are uninteresting, but because they are **physically
unavailable to an individual without an exchange contract and a rack**
(see §6). Where those techniques matter, we *document and simulate* them
rather than pretend to run them.

**The feed is Nasdaq TotalView-ITCH 5.0**, replayed from real exchange
capture over MoldUDP64/UDP. Not a vendor-normalized copy — the actual
bytes Nasdaq's matching engine emits. Market-data-only: fills come from
the execution simulator, not a live venue.

**North star:** learning + resume artifact. The end result should be
defensible in an HFT interview: correct, measured, honestly scoped, and
architected the way real systems are.

---

## 2. The core insight that shapes every priority

**Under replay there is no network and no exchange floor.** Packets come
off a local UDP socket fed by our own simulator. That inverts the
reasoning this section used to carry (which assumed a ~5,000µs crypto
WebSocket round-trip, against which your own code was invisible):

**Decode + book-update cost is now the entire measurable number.** The
nanosecond craft stops being *only* a learning exercise and becomes the
thing actually under measurement. There is nowhere to hide.

Two goals, both first-class:

1. **Nanosecond craft** (ring buffers, order book, cache discipline,
   lock-free hand-offs, latency measurement). Built to production
   standard, and now genuinely measurable end-to-end.
2. **Correctness of order book state**, especially **sequence-gap
   detection and recovery**. Feeds drop packets: the stream stays live,
   the sequence number jumps, and you silently missed messages — your
   book is now wrong. On ITCH this is detected at the **MoldUDP64** layer
   and repaired by **retransmit request** (ITCH has no snapshot message).
   A **first-class module**, not a footnote.

Both goals reinforce each other. Neither is sacrificed for the other.

**What replay still cannot measure** (be honest, see feed-handler.md §7):
kernel-bypass savings (`recvmmsg` pays syscall + softirq costs a
Solarflare/DPDK box doesn't), microburst arrival dynamics that drive
p99.9, and anything about colocation or the physical layer.

---

## 3. Threading model — single hot thread, offload the cold path

**The rule:** the hot path (the critical trade path) should be as few
threads as possible — ideally **one**. Cross-thread hand-offs are only
added where work *cannot* share the hot thread.

### Why fewer threads is lower latency

Every time a message crosses from one core to another via a queue, the
cache line holding the queue's indices is modified by the producer core,
which **invalidates** it in the consumer core; the consumer then pulls
it back across the interconnect. That coherency round-trip is
**~40–100ns per hop**, plus jitter. Four pipelined stages = three hops =
hundreds of ns and added variance spent purely on plumbing.

Thread-per-stage (the textbook diagram) is a **throughput/pipelining**
optimization: each stage works on a different message simultaneously, so
messages-per-second goes up — but **per-message latency goes up too**.
HFT on the critical path wants the opposite. So we do *not* put a thread
boundary between parse → book → strategy → order.

### Where threads *are* necessary (the Optiver rule)

Multithreading is necessary for work that **cannot** run on the hot
thread without wrecking it:

- **Blocking / slow work:** disk logging, DNS, socket reconnect, REST
  snapshot fetch. Doing this on the hot thread stalls it for
  milliseconds. **Must** be offloaded.
- **Independent I/O:** the socket read and the order-send write are their
  own threads so the strategy thread never blocks on a syscall.

### The threads this system actually runs

| Thread | Role | Hot? |
|---|---|---|
| **Socket reader** | `recvmmsg` on the UDP feed socket, timestamps packets at receipt, hands them to the hot thread | I/O |
| **Hot thread** | `decode → book update → strategy → order build → risk → simulated fill` as plain function calls | **HOT** |
| **Logger / telemetry** | Drains latency samples & events to disk/stdout off the hot path | Cold |
| **Recovery** | Issues MoldUDP64 retransmit requests on gap detection | Cold |

> The **feed simulator** (BinaryFILE → MoldUDP64 → UDP) is a *separate
> process*, not a thread of the engine. Keeping it out-of-process is what
> makes the socket read real rather than a function call.
>
> There is **no order-sender thread**: ITCH is market-data-only, so fills
> come from the execution simulator on the hot thread. Keep the seam
> anyway (ARCHITECTURE §5) — it costs nothing and documents the boundary.

**Ring buffers (SPSC, lock-free, cache-line padded) connect hot↔cold and
I/O↔hot** — that's where you build and learn the lock-free primitive,
because that's where it belongs. You do **not** sprinkle queues between
hot stages where they would only add latency.

> Being able to explain *why the cold path gets the queue and the hot
> path doesn't* is a strong interview signal. That's the whole point of
> building it this way.

---

## 4. Instrument-agnostic core, venue-specific edges

**Design the core to be instrument-agnostic; keep only the edges
venue-specific.** This is nearly free and dramatically more credible.

The trick is the **normalized internal event** — venue-neutral:
`SymbolId`, integer-tick `price_t`, `qty_t`, `Side`. Nothing in it says
"Nasdaq" or "ITCH".

> ⚠️ The current `BookDelta` **fails this test**: its `new_qty`
> ("absolute resting qty at a price level") is an L2 concept that cannot
> express ITCH's order-by-order messages. Replacing it with a tagged union
> over `{Add, Execute, Cancel, Delete, Replace}` is the blocking decision
> in feed-handler.md §1.

```
                    ┌─────────── venue-specific ───────────┐
  ITCH 5.0 bytes ─▶ │ Feed Handler │ ... normalizes to ... │ ──▶ Event
   (MoldUDP64/UDP)  └──────────────────────────────────────┘         │
                                                                      ▼
  ┌──────────────────── instrument-agnostic core ─────────────────────────┐
  │  Order Book │ Strategy │ Risk │ Latency Harness │ Ring Buffers          │
  └───────────────────────────────────────────────────────────────────────┘
                                                                      │
                    ┌──────── venue-specific ────────┐                ▼
  Simulated fills ◀ │ Execution Simulator            │ ◀── normalized Order
                    └────────────────────────────────┘
```

- **Feed handler** is the *only* component that knows ITCH. Swap in a CME
  MDP 3.0 handler that emits the same `Event` and the rest is unchanged.
- **`SymbolTable`** maps `stock_locate` → `SymbolId` (see types.md); ITCH
  hands you a dense integer id already.
- **Order book / strategy / risk / latency harness** are agnostic by
  construction.
- **Execution simulator** fills against the replayed book and implements
  the interface a live order gateway *would* — so a gateway drops in later
  without touching strategy or risk.

> Don't build "an ITCH system." Build a **trading core with a feed adapter
> bolted on.** Same work now, ports later.

---

## 5. Data flow (Phase 1: full loop to simulated fills)

```
 MoldUDP64/UDP packets   (from the feed simulator process)
      │  (socket reader thread; recvmmsg, stamps recv_ts)
      ▼
 ┌──────────────┐   normalized L3 events (Add/Exec/Cancel/Delete/Replace)
 │ Feed Handler │───────────────────────────────────────┐
 └──────────────┘   + MoldUDP64 sequence-gap detection  │
      │  gap? ──▶ [Recovery: retransmit request]         │
      ▼                                                  │
 ┌──────────────┐                                        │
 │  Order Book  │  price-time-priority book state        │
 └──────────────┘                                        │
      │  top-of-book / book view                         │
      ▼                                                  │
 ┌──────────────┐                                        │
 │   Strategy   │  market-making signal → desired quotes │
 └──────────────┘                                        │
      │  desired orders                                  │
      ▼                                                  │
 ┌──────────────┐                                        │
 │     Risk     │  pre-trade checks (position, price     │
 └──────────────┘   reasonability, rate limit)           │
      │  approved orders                                 │
      ▼                                                  │
 ┌──────────────────────┐                                │
 │ Execution Simulator  │  fill model + PnL + position   │
 └──────────────────────┘                                │
      │                                                  │
      ▼                                                  ▼
   PnL / position updates                    Latency taps between every
   (fed back to Strategy & Risk)             stage → Logger/telemetry
```

In **Phase 1** the "Order Gateway → real exchange" is replaced by an
**Execution Simulator** (fill modeling, PnL, position). No auth, no real
money, complete end-to-end loop. The gateway is designed with the same
interface the simulator implements, so a live gateway drops in later.

---

## 6. Realism scope & phasing

### Phase 1 — get it running (the resume artifact)
Portable (macOS dev / Linux measure), correct, measured. Single hot
thread; normalized agnostic core; real **L3 order book** driven by real
ITCH; **MoldUDP64 sequence-gap detection + retransmit recovery**; real
latency measurement (monotonic-clock taps between stages, via
`platform::now_ns()`); core pinning where the OS allows (Linux);
cache-line discipline; full loop to **simulated fills**. A complete,
honest, production-shaped system.

The **feed simulator** (BinaryFILE → MoldUDP64 → UDP, with injectable
gaps / reorders / duplicates) is Phase 1, not an extra. It is what makes
the socket read real and the gap path testable. *(This was previously
scoped as a Phase-2 "pure upside" item; adopting ITCH promotes it to the
primary feed.)*

### Phase 2 — "I understand the real stack" (extension, pure upside)
- **Paced replay** at recorded inter-arrival times, so microbursts are
  reproduced and p99.9 means something.
- **A/B feed arbitration** — two simulator instances, independent injected
  drops, arbitrate by sequence. This is what real ITCH consumers do.
- A **`docs/` writeup** characterizing the latency budget and stating
  honestly *where kernel bypass (DPDK/OpenOnload) would and wouldn't
  help*, and what replay structurally cannot measure.

> **Why phase it this way:** kernel bypass, FPGA, hardware timestamping,
> and co-location require an exchange contract and a rack. **Knowing why
> *not* to use something, and proving you understand it, is a stronger
> signal than a half-broken DPDK integration.** Resume line:
> *"characterized the latency budget and documented where kernel-bypass
> would and wouldn't help."*
>
> Note the honest boundary (feed-handler.md §7): on loopback, `recvmmsg`
> pays syscall + softirq costs that a kernel-bypass NIC eliminates — so
> the *savings* from bypass are exactly the thing this setup cannot
> measure. Say that, don't estimate it.

---

## 7. Cross-cutting rules (from README, reaffirmed)

- Decide data representations **before** writing structures that use them.
- **Don't claim "lock-free" / "sub-µs" in a comment until it's measured.**
- One module at a time, each with tests, each measured before optimizing.
- Develop on macOS (correctness), **measure on Linux** (`build.sh perf`,
  pinned core). Mac latency numbers are not meaningful.
- Keep new `#ifdef`s out of the hot path — isolate in `platform.hpp`.

---

## 8. Module index

| Module | File | Phase | Status |
|---|---|---|---|
| Types & SymbolTable | [types.md](types.md) | 1 | ✅ started |
| Feed Handler | [feed-handler.md](feed-handler.md) | 1 | 🚧 parsing done, sequencing TODO |
| Ring Buffer (SPSC) | [ring-buffer.md](ring-buffer.md) | 1 | ⬜ not started |
| Order Book | [order-book.md](order-book.md) | 1 | ⬜ not started |
| Strategy | [strategy.md](strategy.md) | 1 | ⬜ not started |
| Risk | [risk.md](risk.md) | 1 | ⬜ not started |
| Execution Simulator | [execution-simulator.md](execution-simulator.md) | 1 | ⬜ not started |
| Order Gateway (live) | [order-gateway.md](order-gateway.md) | 1.5 | ⬜ deferred |
| Latency Harness | [latency-harness.md](latency-harness.md) | 1 | ⬜ not started |
| Threading & Wiring | [threading.md](threading.md) | 1 | ⬜ not started |

Legend: ✅ done · 🚧 in progress · ⬜ not started
