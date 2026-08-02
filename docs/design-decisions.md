# Design Decisions & Tradeoffs

The design decisions in this engine and the reasoning behind each: what was
chosen, what the alternatives were, why this one, and the conditions under which
the other choice wins.

Each decision below is one that could reasonably have gone the other way. Where
a decision was made wrongly the first time and corrected — §5 and §4 in
particular — the original reasoning and the correction are both recorded.

---

## 1. Nasdaq TotalView-ITCH over a crypto feed

**Decision:** Build against Nasdaq TotalView-ITCH 5.0 (US equities), not a
crypto exchange WebSocket feed.

**Why:**
- **It is the class of protocol equities firms actually run on** —
  exchange-native binary feeds: ITCH (Nasdaq), PITCH/PILLAR, with OUCH for
  order entry. A JSON feed exercises none of the same skills.
- **Order-by-order (L3), not aggregated (L2).** ITCH delivers every individual
  order's lifecycle (add / execute / cancel / delete / replace), keyed by order
  reference number. That forces the real data-structure problems: a ref→order
  index, per-level FIFO queues, price-time priority, queue-position modeling. A
  typical crypto L2 feed hands over pre-aggregated price levels and hides all of
  it.
- **Binary protocol, fixed-layout messages, big-endian on the wire.** Real
  parsing discipline: byte-swapping, `stock_locate` indexing, no allocation on
  the hot path.
- **Deterministic replay.** Captured and replayed from start-of-day; no live
  network, no rate limits, reproducible. Under replay, book update cost is a
  first-order term in the only latency number available — which makes it a clean
  thing to measure.

**The `A`-vs-`E` asymmetry that shapes everything:** `A` (add) carries
side/price/shares plus a new ref; `E` (execute) carries only a ref and a share
count — **no price**. Price must be recovered from the ref index. This asymmetry
is precisely why the ref index is mandatory rather than an optimisation.

**When crypto would win:** if the goal were to actually trade — open APIs, no
market-data fees, no colocation, 24/7. Crypto L3 also exists on some venues, so
the L2-vs-L3 point is about typical access, not a hard rule.

---

## 2. Order-by-order (L3) book, not price-level (L2)

**Decision:** Model individual orders and aggregate them into levels, rather
than tracking net quantity per price.

**Why:** ITCH is L3, so this is forced. But it is also the better model: with
L3, which orders sit at a level and in what order they arrived is known, which
makes **queue position** modelable — how much size rests ahead of a given order
at its price. That is the single biggest realism upgrade available to an
execution simulator, and it is only possible order-by-order.

**Tradeoff:** far more state (millions of orders/day, most short-lived) and a
ref→order index on the hot path. An L2 book is a fraction of the memory and
code. That cost buys queue-position realism, and the feed demands it regardless.

---

## 3. Preallocated object pool + free list, not per-order `new`/`delete`

**Decision:** All orders live in one preallocated `std::vector<RestingOrder>`
(`pool_`), sized once at construction. Allocation pops a slot index off a LIFO
free list; freeing pushes it back. **No heap allocation on the hot path.**

**Why:**
- `new`/`malloc` has **unbounded, unpredictable latency** — locks, page faults,
  fragmentation. Poison for a path where p99 matters, not just the average.
- The pool gives **O(1), deterministic** alloc/free — index bookkeeping, no
  syscalls.
- Indices (`uint32_t`) rather than pointers: smaller (4 B vs 8 B),
  cache-friendlier, and stable across vector growth, where pointers would
  dangle.

**Why LIFO, not FIFO:** the most-recently-freed slot is the most cache-warm —
its line was just touched when the order was unlinked. LIFO hands that warm slot
back next; FIFO would hand out the coldest slot, longest idle and most likely
evicted. LIFO also needs one pointer instead of two. Slots are interchangeable,
so there is no ordering requirement to violate. **Contrast:** the per-level
order *queue* must be FIFO, because price-time priority demands it — different
structure, different requirement.

**Tradeoff:** the pool must be sized for the day's peak live-order count up
front. Overflow drops and counts (`pool_full_drops_`) rather than growing —
growing on the hot path is the exact thing being avoided. The counter makes a
mis-size observable instead of silent.

---

## 4. Open-addressed hash index (`ref → slot`), not `std::unordered_map`

**Decision:** Custom open-addressing (linear-probe) table with backward-shift
deletion, sized to a power of two.

**Why:**
- This lookup fires on **~60% of all messages** (every E/C/X/D/U) — the hottest
  single operation in the book, not plumbing.
- `std::unordered_map` is **node-based**: each entry is a separate heap
  allocation chained by pointers, giving a cache miss per lookup and an
  allocation per insert. Open addressing stores entries inline in one contiguous
  array.
- **Power-of-two capacity** means indexing with a bitmask (`h & mask`) instead
  of a modulo, avoiding integer division.
- A **good mixing hash** (fibonacci/murmur-style finalizer) spreads
  roughly-monotonic refs across buckets so probe chains stay short.

**Tradeoff:** open addressing degrades badly past ~70% load, so it is sized
generously (`2 * pool_capacity`, ~50% max load) — memory traded for speed.
Deletion is the hard part: backward-shift or tombstones, to keep probe chains
valid. Tombstones are simpler but accumulate and slow probes; backward-shift
keeps the table clean at the cost of more work per erase, and of the subtlety
described next.

### The bug this design invited

The backward-shift loop computed distances through a lambda capturing `hole` by
reference, while reassigning `hole` inside the loop. After the first shift, the
comparison measured from a different origin than it started with, so entries
needing relocation were skipped — and `find`, which stops at the first empty
slot, then hit the stale hole and gave up. Refs beyond it became permanently
unreachable.

Minimal case: three refs colliding into one bucket; erase the first, and the
*third* is orphaned. It requires a probe chain of length ≥ 3 to appear, which is
why every hand-written unit test missed it.

On a real replay: 543 orders whose delete/execute could no longer resolve stayed
resting forever, leaving **6 of 7 books crossed** — TSLA showing bid \$644.00
against ask \$624.50. A market maker quoting off that book is quoting a \$19.50
phantom arbitrage.

Two things about how it was found are worth recording. First, the symptom
surfaced during a *latency* investigation — the crossed books turned up only
because a full-fixture replay was being sanity-checked before publishing
benchmark numbers. Second, what settled it was **differential testing**: an
independent Python scan of the raw ITCH bytes proved the feed contained zero
messages referencing unknown refs, which meant the data was self-consistent and
the book had to be losing orders on its own. The fix is verified by a fuzz test
against `std::unordered_map` checking every live entry after each of 20 000
operations.

**The correct invariant:** shift `slots_[j]` into `hole` only when its ideal
bucket falls *outside* the cyclic interval `(hole, j]` — recomputing both
distances from the *current* hole every iteration, since the hole moves.

**Why not tombstones:** they are simpler and immune to this class of bug, but
they accumulate, lengthen probes, and require periodic rehashing.
Backward-shift keeps probes short at the cost of exactly the subtlety above.
Each design invites its own failure mode; this one's is now covered by tests.

### The `ref == 0` subtlety

Empty slots are marked `ref == 0`, making 0 an illegal key. **Nasdaq's Trade
(`P`) message carries ref = 0** — hidden-order executions, zeroed for anonymity.
But `P` is a *trade print, not an order-book event*, so it must never reach the
book. `insert` **asserts `ref != 0`** as a tripwire that catches a feed-handler
routing bug at the source rather than as silent corruption later.

---

## 5. Dense fixed-window array of price levels, not `std::map<price, qty>`

**Decision:** Per side, a contiguous array of `Level` structs indexed by tick
offset from a **fixed** `base_price_`, set at construction from a per-symbol
reference price. Price is already an integer tick, so price→level is array
indexing, not a tree lookup. The window is 32 768 ticks (±\$163.84 around the
reference, ≈±27% on a \$600 stock). Prices outside it are "far": tracked in the
pool and ref index so their later delete/execute resolves, but given no level
and no touch bit.

**Why an array at all:**
- `std::map` (red-black tree) is the correct-but-slow baseline: O(log n) per
  update, a pointer-chase per node (poor cache behavior), and a heap allocation
  per new level.
- An array indexed by tick is **O(1) update and O(1) best** (with the bitmap
  below), and cache-friendly — levels near the touch are contiguous.

### Why fixed, not sliding — this was gotten wrong first

The original design was a circular buffer whose origin advanced as price
drifted, which sounds like the more sophisticated choice. It was not, for one
specific reason: **the trigger was wrong.** `add_order` re-centred the window
whenever an incoming order landed outside it. So a single deep resting order —
someone's standing bid \$25 below the market — dragged the entire window onto
itself, **evicting the near-mid book**, and the next normal add dragged it back.

Measured on a real session: **2 124 recentres per replay, ~14 000 resting orders
evicted**, and an equal number of subsequent lookups failing to find their
order.

It was also the entire latency tail. Each recentre ran `rebuild_bitmap()`, an
O(window) rescan, and the counts matched 1:1 — 2 124 recentres against ~2 150
samples over 5 µs. Removing it took **p99.9 from 15 981 ns to 590 ns** on
Linux/x86 — a 27× reduction, with p50 and p99 unchanged. That combination is the
signature of removing a rare expensive path rather than speeding up the common
one.

**What real desks do:** most equities books use a fixed absolute price array
sized from the previous close, because the memory is irrelevant (a few MB) and
it removes an entire class of bug. Sliding windows appear where price ranges are
genuinely unbounded (futures, FX) or memory is constrained (FPGA) — and there
the window follows **the touch**, with hysteresis, never a single incoming
order. An out-of-window order goes to the overflow path; it is not a reason to
move the window.

**Tradeoff:** the fixed window cannot follow a symbol that moves more than ±27%
intraday. That is the correct behaviour — past LULD halt bands the exchange has
already stopped trading, and "stop quoting" beats "silently reorganise the book
mid-event."

**Sizing:** 32 768 levels × 24 B × 2 sides ≈ 1.6 MB per symbol. Outside the
window, orders take the far path and are counted (`far_orders_`); in production
a symbol whose touch approached the window edge would be flagged rather than
re-windowed.

**When `std::map` would win:** genuinely unbounded depth at arbitrary prices, or
a book so sparse the array wastes memory. For a near-touch HFT book the
array/bitmap wins decisively.

---

## 6. Hierarchical bitmap for O(1) best-bid/ask, not a scan or a scalar

**Decision:** Per side, a **three-tier** bitset marking which levels are
non-empty: 512 detail words (`bits_[512]` = 32 768 bits, one per level) → 8 mid
words (`mid_[8]`, bit *j* = "detail word *j* is non-zero") → one 64-bit summary
(bit *k* = "mid word *k* is non-zero"). Best bid/ask costs three `clz`/`ctz`
instructions.

**The alternatives, and why each loses:**
- **Naive scalar cache** (`best_bid_idx_`) alone: O(1) to *read*, but when the
  touch level empties on a delete, finding the next-best is an O(window) scan
  downward. Deletes at the touch are extremely common, so that scan lands
  squarely on the hot path.
- **Scan the level array** on every query: O(window). Worse.
- **Ordered tree** (`std::map::begin()`): O(1) read but O(log n) erase and poor
  cache behavior — the thing the array design exists to avoid.
- **Bitmap:** "find highest/lowest non-empty level" becomes "find highest/lowest
  set bit among 32 768." Each tier narrows the search by a factor of 64 with one
  `clz`, so three dependent loads and three instructions land on the exact bit.
  O(1), no data-dependent loop. It composes perfectly with the dense array
  because it is just a parallel bit-index over the same level indices.

**The invariant:** a tier's bit is set ⟺ the word below it is non-zero, at every
level. Adds set the detail bit and unconditionally set the mid and summary bits
above it. Deletes clear the detail bit, then clear the mid bit *only if* that
whole detail word hit zero, then the summary bit *only if* that whole mid word
hit zero.

**The asymmetry is the crux:** adding always makes a word non-zero, so it can
set upward unconditionally; removing only *sometimes* empties a word, so the
clear must cascade conditionally. Getting that nesting wrong either reports a
phantom price (bit left set) or loses a live level (bit cleared too eagerly).
Seven tests pin exactly this, including "sibling in the same word still
occupied" at each tier.

**Why 32 768 = 512 × 64, and why three tiers:** two tiers cover 64 × 64 = 4 096
levels, only ±\$20.48 at penny ticks — far too narrow for a \$600 stock, and
measurably so: it forced hundreds of window moves per session. Widening to
±\$163.84 needs 512 detail words, and 512 exceeds what a single 64-bit summary
can index, hence the middle tier. Each tier multiplies reach by 64, so *n* tiers
cover 64ⁿ levels; three reach 262 144, well past anything needed here. This is
the standard hierarchical-bitmap trick, also used in OS schedulers, buddy
allocators, and find-first-set structures.

**On caching the touch anyway:** a cached scalar *on top of* the bitmap is a
reasonable later optimisation — the touch is read far more often than it moves —
but it requires every path that empties a level to maintain it, which is where
that design goes subtly wrong.

---

## 7. Integer tick prices, not floating-point

**Decision:** `price_t = int64` in ticks; `price = n * tick_size`, with
`tick_size` looked up per symbol.

**Why:** floating-point prices have exact-equality and fragile-keying problems —
a `double` cannot safely be a map key or array index, and rounding drift
accumulates. Integer ticks are exact, fast, and **directly usable as an array
index**, which is what makes the dense level array possible. This is standard in
real trading systems.

**Tradeoff:** a per-symbol tick_size/scale table must be carried, with
conversion at the feed boundary. Cheap, and done once per message.

---

## 8. Driven directly by the decoder — no intermediate `Event` type

**Decision:** The ITCH decoder's `switch` calls `add_order`/`delete_order`/…
directly. There is no normalized `Event` struct between decoder and book.

**Why:** an intermediate event type means building an `Event`, then
destructuring it again in the book — an extra copy and a cache round-trip on the
hot path, for no functional gain in a single-consumer design. The method-call
seam is the same abstraction boundary without the data-shuffling cost.

**The `BookDelta` history:** an earlier iteration had a `BookDelta` struct as the
feed's output — a **price-level (L2)** delta (`price`, `new_qty`), the natural
shape for a Coinbase-style feed that hands over aggregated levels. Switching to
**Nasdaq ITCH (L3)** killed it: ITCH is order-by-order, so `AddOrder` carries an
*order reference* and `OrderExecuted` says "order `ref` traded N shares"
**without a price at all**. There is no price-level delta to emit, and `new_qty`
is meaningless per-order. `BookDelta` was deleted rather than reshaped: with one
venue, one thread, and one consumer, the normalized-event abstraction earned
nothing.

This is a **made-for-speed, single-venue** decision. It trades the
venue-agnostic generality a normalized event would buy for the copy and dispatch
it would cost.

**When an `Event` type earns its place:** if multiple independent consumers need
the same normalized stream (book + tape + recorder), if decode must be decoupled
from apply across a queue or thread boundary, or if an **L2 venue** is added
whose native shape *is* a price-level delta. Then normalization pays for itself.

---

## 9. `noexcept` on the hot path

**Decision:** book mutation and query methods are `noexcept`.

**Why:** exceptions on the hot path are a non-starter — unpredictable cost, and
the book cannot meaningfully recover mid-message anyway. `noexcept` also lets the
compiler skip unwinding machinery and can enable more optimization. Errors that
*can* happen (pool full, ref not found, price out of band) are handled by
**counters plus a graceful no-op**, not exceptions — which is also what keeps the
engine robust against dropped or corrupt packets.

---

## 10. Assertions as invariant tripwires (debug-loud, release-free)

**Decision:** Precondition and invariant violations (`ref != 0`, no duplicate
ref, power-of-two sizes, probe-table-not-full) are `assert`s.

**Why:** ITCH *guarantees* these invariants, so a violation is a bug in this
code or a corrupt feed, not a normal case. `assert` fires immediately at the
source in debug/ASan builds and compiles to zero in release. This is the standard
discipline: **invariants asserted in debug, assumed in release.** The alternative
(always-check plus return codes) adds hot-path cost for cases that cannot happen
— that treatment is reserved for genuinely external conditions like dropped
packets.

---

## 11. Measuring honestly — and what the measurement found

**Decision:** Per-message latency is measured with the CPU cycle counter,
fenced, compiled in only for the benchmark build, over 862 k applied messages of
real ITCH, and reported as a distribution across 20 runs. Full methodology in
[benchmarks.md](benchmarks.md).

**The measured result:** **p50 123 ns / p99 426 ns / p99.9 590 ns** on a pinned
core of a Xeon 8280 at 2.694 GHz; instrumentation overhead 34–36 cycles, so net
≈110 ns. p50 spans 120.24–126.18 ns across all 20 runs.

**The technique points that matter:**
- **Cycle counter, not `clock_gettime`.** Per-message work is tens of ns; a
  ~25 ns clock read would dominate what it measures.
- **`rdtsc` is fenced** (`lfence; rdtsc; lfence`). `rdtsc` is not serialising —
  unfenced, the CPU reorders work across it and the measurement window fails to
  bracket the code under test.
- **Zero cost in production.** The decode path is templated on `<bool Timing>`;
  `decode<false>` compiles the instrumentation *out* via `if constexpr` — absent
  from the emitted code, not merely branched around.
- **Median of per-run percentiles with the range**, never the mean and never
  samples pooled across runs — pooling would hide exactly the run-to-run
  variance the repeated runs exist to expose.
- **Known limits:** no NIC receive, no wire-to-book, a KVM guest rather than bare
  metal, and the `cycles` PMU gated on that guest, so **IPC is not claimed**.

### On comparing this number to anything

There is no published per-message book-apply latency to rank against; firms do
not release it. The public figures — sub-microsecond, commonly 1–5 µs — are
almost always **tick-to-trade**: NIC in → decision → NIC out, covering network
stack, decode, book, strategy, risk, and encoding. 123 ns is *one stage inside
that path*, so the comparison is not available in either direction.

What can be defended is the magnitude, from first principles: ~110 ns at
2.694 GHz is ~300 cycles for a hash probe, a level access, a list unlink, and a
three-tier bitmap update. An L3 hit is ~40 cycles and a DRAM miss ~200–300, so
that budget is consistent with a couple of cache misses plus real work. The 73%
cache-miss rate corroborates it. That reasoning rests on hardware behaviour
rather than on an unpublished benchmark.

Two further caveats stated plainly: the p50 is the number to quote, not the
74.8 M msgs/sec throughput, which is dominated by messages that early-out on a
non-watchlist symbol and mostly measures the framing loop. And **no optimisation
pass has been done** — every improvement so far removed a pathology; nothing has
yet targeted the common path.

### Two bugs that only the measurement caught

Both were found because a latency number was being prepared for publication, and
both turned out to be *correctness* bugs, not performance ones:

1. **The recentre policy** (§5) — a fat p99.9 traced to 2 124 window moves per
   replay, each evicting resting orders. The tail was the symptom; silent book
   corruption was the disease. Fixing it took p99.9 from 15 981 ns to 590 ns
   (27×) while leaving p50 and p99 unchanged.
2. **`RefIndex::erase`** (§4) — a sanity check before publishing found 6 of 7
   books ending **crossed**. Root cause was a backward-shift deletion bug
   orphaning entries in collision chains.

### The methodology lesson: profiles point, they don't prove

The first hypothesis for the tail was wrong, and `perf` *reinforced* the wrong
answer — it showed a page-fault chain under the book constructor, which looked
conclusive. It was not, because that code path returns before the timing block
and was never in the histogram at all.

What actually settled it: (a) checking whether the proposed cause was even
inside the measured region, which alone eliminated the page-fault theory; (b)
direct counter instrumentation showing a 1:1 match between recentres and tail
samples, which no profile could have shown because `recenter` inlined into
`add_order` under LTO; and (c) reproducing the tail on a second OS, which a
first-touch-fault explanation cannot survive.

---

## Reference: message-path semantics

Two behaviours worth stating explicitly, since both follow from exchange
semantics rather than from implementation choice.

**What happens on an `E` (execute) message:** ref-index lookup → recover the
order, taking price and side from the stored `RestingOrder` → find the level →
reduce quantity → if it reaches zero, unlink from the level's FIFO, free the pool
slot, and clear the bitmap bit if the level emptied (cascading up the tiers) →
erase from the ref index.

**Why replace (`U`) loses time priority:** ITCH `U` is delete-old plus add-new,
so the new ref goes to the *tail* of its level's FIFO. That is real exchange
semantics, not an implementation shortcut.

**ITCH provides no snapshots.** It is pure incremental from session start.
Mid-session joins use **GLIMPSE**, a separate TCP snapshot service returning the
book plus the sequence number it corresponds to; MoldUDP64 retransmission covers
small gaps. A diverged book is never *repaired* in place — the correct response
is to stop quoting the symbol, re-snapshot, and resume.

## How correctness is established

Layered, weakest to strongest:
- Unit tests pin invariants (the bitmap cascade, probe chains).
- A **differential fuzz** checks `RefIndex` against `std::unordered_map` after
  every one of 20 000 operations.
- A full replay asserts no crossed book and `not_found == 0`.
- An **independent Python scan of the raw ITCH bytes** provides ground truth the
  C++ can be checked against.

The last of these is what caught the erase bug: a second implementation, written
against the spec independently, exists specifically to disagree with the first.

## Cross-references
- [order-book.md](order-book.md) — the committed book design in detail.
- [feed-handler.md](feed-handler.md) — ITCH decode and message routing.
- [benchmarks.md](benchmarks.md) — full measurement methodology and results.
- [latency-harness.md](latency-harness.md) — how the timing tap is built.
- [design/execution-simulator.md](design/execution-simulator.md) —
  queue-position modeling (why L3 matters).
