# Interview Prep — Design Decisions & Tradeoffs

A cheat sheet of the design decisions in this project and the reasoning
behind each — the kind of thing an HFT / low-latency interviewer probes.
The goal is to be able to explain *why I chose X over Y*, name the
tradeoff, and know when the other choice would win. Reciting an
architecture impresses no one; **owning a decision** does.

For each item: the decision, the alternatives, why this one, and when
you'd choose differently.

---

## 1. Nasdaq TotalView-ITCH over a crypto feed (e.g. Coinbase)

**Decision:** Build against Nasdaq TotalView-ITCH 5.0 (US equities), not a
crypto exchange WebSocket feed.

**Why:**
- **It's what the target shops actually trade.** HFT/market-making firms
  run on exchange-native binary protocols — ITCH (Nasdaq), PITCH/PILLAR,
  OUCH for order entry. Demonstrating you can handle a real one is
  directly relevant; a JSON crypto feed is not.
- **Order-by-order (L3), not aggregated (L2).** ITCH delivers every
  individual order's lifecycle (add / execute / cancel / delete /
  replace), keyed by order reference number. That forces the *real*
  data-structure problems: a ref→order index, per-level FIFO queues,
  price-time priority, queue-position modeling. A typical crypto L2 feed
  hands you pre-aggregated price levels and hides all of that — much less
  to build, much less to learn.
- **Binary protocol, fixed-layout messages, big-endian on the wire.**
  Real parsing discipline: byte-swapping, `stock_locate` indexing, no
  allocation on the hot path. JSON parsing is the opposite of the skill
  being demonstrated.
- **Deterministic replay.** ITCH is captured and replayed from
  start-of-day; no live network, no rate limits, reproducible. Under
  replay, **book update cost is a first-order term in the only latency
  number you have** — which makes it a clean thing to measure.

**When crypto would win:** if the *goal* were to actually trade (crypto
has open APIs, no market-data fees, no colocation, 24/7). For a
resume/learning project targeting equities/HFT firms, ITCH is the
higher-signal choice. Also: crypto L3 does exist (e.g. some venues'
full-order-book feeds) — the L2-vs-L3 point is about *typical* access,
not a hard rule.

**Follow-up you should be ready for:** "What's in an ITCH `A` message vs an
`E`?" → `A` (add) carries side/price/shares + a new ref; `E` (execute)
carries only a ref + shares (no price — you recover price from your index).
This asymmetry is *why* the ref index is mandatory.

---

## 2. Order-by-order (L3) book, not price-level (L2)

**Decision:** Model individual orders, aggregate into levels — don't just
track net qty per price.

**Why:** ITCH is L3, so you must. But it's also *better*: with L3 you know
**which orders sit at a level and in what order they arrived**, so you can
model **queue position** — how much size rests ahead of you at your price.
That's the single biggest realism upgrade for an execution simulator, and
it's only possible order-by-order.

**Tradeoff:** far more state (millions of orders/day, most short-lived) and
a ref→order index on the hot path. An L2 book is a fraction of the memory
and code. You pay that cost specifically to get queue-position realism and
because the feed demands it.

---

## 3. Preallocated object pool + free list, not per-order `new`/`delete`

**Decision:** All orders live in one preallocated `std::vector<RestingOrder>`
(`pool_`), sized once at construction. Allocation = pop a slot index off a
LIFO free list; free = push it back. **No heap allocation on the hot path.**

**Why:**
- `new`/`malloc` has **unbounded, unpredictable latency** — locks,
  page faults, fragmentation. Poison for a hot path where you care about
  p99, not just average.
- The pool gives **O(1), deterministic** alloc/free — just index
  bookkeeping, no syscalls.
- Indices (`uint32_t`), not pointers: smaller (4B vs 8B), cache-friendlier,
  and **stable across vector growth** if you ever resize (pointers would
  dangle).

**Why LIFO free list (not FIFO):** the most-recently-freed slot is the most
**cache-warm** — its line was just touched when the order was unlinked.
LIFO hands that warm slot back next; FIFO would hand out the coldest slot
(longest idle → likely evicted). LIFO also needs one pointer, not two.
Slots are interchangeable (no ordering requirement), so we're free to pick
the cache-optimal layout. **Contrast:** the per-level order *queue* MUST be
FIFO (price-time priority) — different structure, different requirement.

**Tradeoff:** you must size the pool for the day's peak live-order count up
front. Overflow = drop + count (`pool_full_drops_`), not grow (growing on
the hot path is exactly what we're avoiding). The counter makes a mis-size
observable instead of silent.

---

## 4. Open-addressed hash index (`ref → slot`), not `std::unordered_map`

**Decision:** Custom open-addressing (linear-probe) table with
backward-shift deletion, sized to a power of two.

**Why:**
- This lookup fires on **~60% of all messages** (every E/C/X/D/U) — it's
  the hottest single operation in the book, not plumbing.
- `std::unordered_map` is **node-based**: each entry is a separate heap
  allocation, chained by pointers → a cache miss per lookup, and heap
  allocation per insert. Open addressing stores entries **inline in one
  contiguous array** → far better locality, no per-entry allocation.
- **Power-of-two capacity** → index with a bitmask (`h & mask`) instead of
  a modulo (`h % n`), which is a slow integer division.
- A **good mixing hash** (fibonacci/murmur-style finalizer) spreads
  roughly-monotonic refs across buckets so probe chains stay short.

**Tradeoff / cost:** open addressing degrades badly past ~70% load, so it's
sized generously (`2 * pool_capacity`, ~50% max load) — more memory for
speed. Deletion is non-trivial: **backward-shift** (or tombstones) to keep
probe chains valid. Tombstones are simpler but accumulate and slow probes;
backward-shift keeps the table clean at the cost of more work per erase.
We chose backward-shift.

**The bug this actually had (the best story in this document).** The
backward-shift loop computed distances through a lambda capturing `hole` by
reference, while reassigning `hole` inside the loop. After the first shift the
comparison measured from a different origin than it started with, so entries
that needed relocating were skipped — and `find`, which stops at the first
empty slot, then hit the stale hole and gave up. Refs beyond it became
permanently unreachable.

Minimal case: three refs colliding into one bucket, erase the first, the
*third* is orphaned. It needs a probe chain of length ≥ 3 to appear, so it
survived every hand-written unit test.

What it did on a real replay: 543 orders whose delete/execute could no longer
resolve stayed resting forever, leaving **6 of 7 books crossed** — TSLA showing
bid \$644.00 against ask \$624.50. A market maker quoting off that book is
quoting a \$19.50 phantom arbitrage.

Two things are worth saying about how it was found. First, the symptom was a
*latency* investigation — the crossed books turned up only because a full-fixture
replay was being sanity-checked before publishing benchmark numbers. Second,
what settled it was **differential testing**: an independent scan of the raw
ITCH bytes proved the feed contained zero messages for unknown refs, which meant
the data was self-consistent and the book had to be losing orders itself. The
fix is verified by a fuzz test against `std::unordered_map` that checks every
live entry after each of 20 000 operations.

**The correct invariant:** shift `slots_[j]` into `hole` only when its ideal
bucket falls *outside* the cyclic interval `(hole, j]` — recompute both
distances from the *current* hole every iteration, since the hole moves.

**Follow-up you should expect:** "Why not tombstones?" → simpler and immune to
this class of bug, but they accumulate, lengthen probes, and need periodic
rehashing. Backward-shift keeps probes short at the cost of exactly the
subtlety described above. Knowing *which* bug each design invites is the
answer they want.

**The `ref == 0` subtlety (great story):** empty slots are marked
`ref == 0`, so 0 is an illegal key. **Nasdaq's Trade (P) message carries
ref = 0** (hidden-order executions, zeroed for anonymity) — but `P` is a
*trade print, not an order-book event*, so it must never reach the book.
`insert` **asserts `ref != 0`** as a tripwire that catches a feed-handler
routing bug at the source instead of as silent corruption later. This is
the kind of protocol-detail-meets-implementation-invariant that shows real
engineering.

---

## 5. Dense fixed-window array of price levels, not `std::map<price, qty>`

**Decision:** Per side, a contiguous array of `Level` structs indexed by tick
offset from a **fixed** `base_price_` set at construction from a per-symbol
reference price. Price is already an integer tick, so price→level is array
indexing, not a tree lookup. Window is 32 768 ticks (±\$163.84 around the
reference, ≈±27% on a \$600 stock). Prices outside it are "far": tracked in the
pool and ref index so their later delete/execute resolves, but given no level
and no touch bit.

**Why an array at all:**
- `std::map` (red-black tree) is the "correct but slow" baseline: O(log n) per
  update, **a pointer-chase per node** (terrible cache behavior), and a heap
  allocation per new level.
- An array indexed by tick is **O(1) update and O(1) best** (with the bitmap,
  below), and **cache-friendly** — levels near the touch are contiguous.

**Why fixed, not a sliding ring — and this project got it wrong first.** The
original design was a circular buffer whose origin advanced as price drifted,
which sounds like the sophisticated choice. It wasn't, for one specific reason:
**the trigger was wrong.** `add_order` re-centred the window whenever an
incoming order landed outside it. So a single deep resting order — someone's
standing bid \$25 below the market — dragged the entire window onto itself,
**evicting the near-mid book**, and the next normal add dragged it back.
Measured on a real session: **2 124 recentres per replay, ~14 000 resting orders
evicted**, and an equal number of subsequent lookups failing.

It was also the entire latency tail. Each recentre ran `rebuild_bitmap()`, an
O(window) rescan, and the counts matched 1:1 — 2 124 recentres against ~2 150
samples over 5 µs. Removing it took **p99.9 from 15 981 ns to 590 ns** on
Linux/x86 — a 27× reduction, with p50 and p99 unchanged.

**What real desks do:** most equities books use a fixed absolute price array
sized from the previous close, because the memory is irrelevant (a few MB) and
it removes an entire class of bug. Sliding windows appear where price ranges are
genuinely unbounded (futures, FX) or memory is constrained (FPGA) — and there
the window follows **the touch**, with hysteresis, never a single incoming
order. An out-of-window order goes to the overflow path; it is not a reason to
move the window.

**Tradeoff:** the fixed window can't follow a symbol that moves more than ±27%
intraday. That is the correct behaviour — past LULD halt bands the exchange has
already stopped trading, and "stop quoting" beats "silently reorganise your book
mid-event."

**When `std::map` would win:** genuinely unbounded depth at arbitrary prices, or
a book so sparse the array wastes memory. For a near-touch HFT book the
array/bitmap wins decisively.

**Follow-up you should expect:** "How big is the array, and what if price leaves
it?" → 32 768 levels × 24 B × 2 sides ≈ 1.6 MB per symbol; outside it, orders
take the far path and are counted (`far_orders_`), and in production a symbol
whose touch approached the edge would be flagged rather than re-windowed.

---

## 6. Hierarchical bitmap for O(1) best-bid/ask, not a scan or a scalar

**Decision:** Per side, a **three-tier** bitset marking which levels are
non-empty: 512 detail words (`bits_[512]` = 32 768 bits, one per level) → 8 mid
words (`mid_[8]`, bit *j* = "detail word *j* is non-zero") → one 64-bit summary
(bit *k* = "mid word *k* is non-zero"). Best bid/ask = three `clz`/`ctz`
(count-leading/trailing-zeros) instructions.

**Why this is the crux question — "how do you find the best price?":**
- **Naive scalar cache** (`best_bid_idx_`) alone: O(1) to *read*, but when
  the touch level empties on a delete you must **find the next-best**,
  which is an O(window) scan downward. Deletes at the touch are extremely
  common → that scan is the hot path. Not acceptable.
- **Scan the level array** every query: O(window). Worse.
- **Ordered tree** (`std::map::begin()`): O(1) read but O(log n) erase and
  bad cache behavior — the thing the array design exists to avoid.
- **Bitmap:** "find highest/lowest non-empty level" = "find highest/lowest set
  bit among 32 768." Each tier narrows the search by a factor of 64 with one
  `clz`, so three dependent loads and three instructions land on the exact bit.
  **O(1), no data-dependent loop.** Composes perfectly with the dense array
  because it's just a parallel bit-index over the same level indices.

**Key invariant to state:** "a tier's bit is set ⟺ the word below it is
non-zero," at every level. Adds set the detail bit and unconditionally set the
mid and summary bits above it. Deletes clear the detail bit, then clear the mid
bit *only if that whole detail word hit zero*, then the summary bit *only if
that whole mid word hit zero*. **The asymmetry is the crux:** adding always
makes a word non-zero, so it can set upward unconditionally; removing only
*sometimes* empties a word, so the clear must cascade conditionally. Get that
nesting wrong and you either report a phantom price (bit left set) or lose a
live level (bit cleared too eagerly). This is the first thing tests should pin —
there are seven tests here doing exactly that, including "sibling in the same
word still occupied" at each tier.

**Why 32 768 = 512 × 64, and why three tiers:** two tiers cover 64 × 64 = 4 096
levels, which is only ±\$20.48 at penny ticks — far too narrow for a \$600 stock
(measured: it forced hundreds of window moves per session). Widening the window
to ±\$163.84 needs 512 detail words, and 512 exceeds what a single 64-bit
summary can index, hence the middle tier. **The general rule:** each tier
multiplies reach by 64, so *n* tiers cover 64ⁿ levels — 3 tiers reach 262 144,
well past anything needed here. Standard hierarchical-bitmap trick, also used in
OS schedulers, buddy allocators, and `find-first-set` structures.

**Follow-up you should expect:** "Why not just cache `best_bid_idx_`?" → because
the expensive case isn't reading the touch, it's *re-finding* it when the touch
level empties. A cached scalar makes that an O(window) scan. The bitmap makes it
three instructions. A cached scalar *on top of* the bitmap is a reasonable later
optimisation — the touch is read far more often than it moves — but it needs
every path that empties a level to maintain it, which is where that design gets
subtly wrong.

---

## 7. Integer tick prices, not floating-point

**Decision:** `price_t = int64` in ticks; `price = n * tick_size`, tick_size
looked up per-symbol.

**Why:** floating-point prices have **exact-equality / fragile-keying**
problems — you can't safely use a `double` as a map key or array index, and
rounding drift accumulates. Integer ticks are **exact, fast, and directly
usable as an array index** (which is what makes the dense level array
possible). This is standard in real trading systems.

**Tradeoff:** you carry a per-symbol tick_size/scale table and convert at
the feed boundary. Cheap, and done once per message.

---

## 8. Driven directly by the decoder — no intermediate `Event` type

**Decision:** The ITCH decoder's `switch` calls
`add_order`/`delete_order`/... directly. There's no normalized `Event`
struct between decoder and book.

**Why:** an intermediate event type means building an `Event`, then
destructuring it again in the book — an extra copy/allocation and a cache
round-trip on the hot path, for no functional gain in a single-consumer
design. The method-call seam is the same abstraction boundary without the
data-shuffling cost.

**The `BookDelta` history (the ITCH-vs-Coinbase part of the story):** an
earlier iteration had a `BookDelta` struct as the feed's output — a
**price-level (L2)** delta (`price`, `new_qty`), the natural shape for a
Coinbase-style feed that hands you aggregated levels. Switching the feed to
**Nasdaq ITCH (L3)** killed it: ITCH is order-by-order, so `AddOrder`
carries an *order reference* and `OrderExecuted` says "order `ref` traded N
shares" **without a price at all** — there is no price-level delta to emit,
and `new_qty` is meaningless per-order. `BookDelta` was deleted rather than
reshaped: with one venue, one thread, and one consumer, the normalized-event
abstraction earned nothing, so the decoder calls the book directly. This is
a *made-for-speed, single-venue* decision — it trades the venue-agnostic
generality a normalized event would buy for the copy/dispatch it would cost.

**When you'd add an `Event` type (or bring back a delta):** if multiple
independent consumers need the same normalized stream (book + tape +
recorder), if you want to decouple decode from apply across a queue/thread
boundary, or if you re-add an **L2 venue** whose native shape *is* a
price-level delta. Then the normalization pays for itself. Know this
tradeoff — it's a "when does abstraction cost too much" question.

---

## 9. `noexcept` on the hot path

**Decision:** book mutation/query methods are `noexcept`.

**Why:** exceptions on the hot path are a non-starter (unpredictable cost,
and the book can't meaningfully "recover" mid-message anyway). `noexcept`
also lets the compiler skip unwinding machinery and can enable more
optimization. Errors that *can* happen (pool full, ref not found, price out
of band) are handled by **counters + graceful no-op**, not exceptions —
which is also how you stay robust against dropped/corrupt packets.

---

## 10. Assertions as invariant tripwires (debug-loud, release-free)

**Decision:** Precondition/invariant violations (`ref != 0`, no duplicate
ref, power-of-two sizes, probe-table-not-full) are `assert`s.

**Why:** ITCH *guarantees* these invariants, so a violation is a **bug in
your code or a corrupt feed**, not a normal case. `assert` fires
**immediately, at the source**, in debug/ASan builds, and **compiles out to
zero cost** in release. This is the standard HFT discipline: *invariants
asserted in debug, assumed in release.* It's how you "catch bugs upfront"
without paying for checks on the hot path. The alternative (always-check +
return codes) adds hot-path cost for cases that "can't happen" — reserved
for genuinely-external conditions (dropped packets), not internal
invariants.

---

---

## 11. Measuring honestly — and what measurement found

**Decision:** Per-message latency is measured with the CPU cycle counter, fenced,
compiled in only for the benchmark build, over 862 k applied messages of real
ITCH, reported as a distribution across 20 runs. Full methodology in
`docs/benchmarks.md`.

**The measured result:** **p50 123 ns / p99 426 ns / p99.9 590 ns** on a pinned
core of a Xeon 8280 at 2.694 GHz; instrumentation overhead 34–36 cycles, so net
≈110 ns. p50 spans 120.24–126.18 ns across all 20 runs.

**"How does that compare to a real shop?" — the answer is that it doesn't, and
saying so is the strong move.** Firms don't publish per-message book-apply
latency. The public figures — sub-microsecond, commonly 1–5 µs — are
**tick-to-trade**: NIC in → decision → NIC out, covering network stack, decode,
book, strategy, risk and encoding. Your 123 ns is *one stage inside that path*.
Presenting it against a tick-to-trade number, in either direction, is the kind
of error that costs credibility for everything else you say. What you can
defend: what the number measures, what it excludes, and how it was produced.

**What you can say about the magnitude, from first principles:** ~110 ns at
2.694 GHz is ~300 cycles for a hash probe, level access, list unlink and a
three-tier bitmap update. An L3 hit is ~40 cycles, a DRAM miss ~200–300 — so
that budget is consistent with a couple of cache misses plus real work. The
73% cache-miss rate corroborates it. That is a defensible statement because it
reasons from hardware behaviour rather than from an unpublished benchmark.

**Also:** lead with p50, not the 74.8 M msgs/sec throughput — that figure is
dominated by messages that early-out on a non-watchlist symbol and mostly
measures the framing loop. And volunteer that no optimisation pass has been
done; it is true, and it pre-empts the obvious follow-up.

**The points worth stating:**
- **Cycle counter, not `clock_gettime`.** Per-message work is tens of ns;
  a ~25 ns clock read would dominate what it measures.
- **`rdtsc` is fenced** (`lfence; rdtsc; lfence`). `rdtsc` is not serialising —
  unfenced, the CPU reorders work across it and the measurement window doesn't
  bracket the code. Expect to be asked this directly.
- **Zero cost in production.** The decode path is templated on `<bool Timing>`;
  `decode<false>` compiles the instrumentation *out* via `if constexpr` — not
  branched around, absent from the emitted code.
- **Median of 5 runs with the range**, never the mean and never pooled samples —
  pooling would hide exactly the run-to-run variance the runs exist to show.
- **Know what you did NOT measure:** no NIC receive, no wire-to-book, KVM guest
  rather than bare metal, and `cycles` PMU gated on that guest so **IPC is not
  claimed.** Naming your own limits is worth more than one more digit.

**Two bugs that only a measurement caught — this is the story to lead with.**
Both were found because a latency number was being prepared for publication, and
both were *correctness* bugs, not performance ones:

1. **The recentre policy** (§5) — a fat p99.9 traced to 2 124 window moves per
   replay, each evicting resting orders. The tail was the symptom; silent book
   corruption was the disease. Fixing it took p99.9 from 15 981 ns to 590 ns
   (**27×**) while leaving p50 and p99 unchanged — which is exactly the
   signature of removing a rare expensive path rather than speeding up the
   common one, and worth saying out loud.
2. **`RefIndex::erase`** (§4) — a sanity check before publishing found 6 of 7
   books ending **crossed**. Root cause was a backward-shift deletion bug
   orphaning entries in collision chains.

**The methodology point an interviewer will actually care about:** the first
hypothesis for the tail was wrong, and `perf` *reinforced* the wrong answer — it
showed a page-fault chain under the book constructor, which looked conclusive.
It wasn't, because that code path returns before the timing block and was never
in the histogram. What settled it was (a) checking whether the proposed cause was
even inside the measured region, (b) direct counter instrumentation showing a 1:1
match between recentres and tail samples, and (c) reproducing on a second OS,
which a page-fault explanation cannot survive. **Profiles point; they don't
prove.**

---

## Things to have crisp for the interview

- **"What's your p99 latency per message?"** — HFT interviews *start* here.
  **426 ns p99, 123 ns p50**, over 862 k applied ITCH messages on a pinned x86
  core; ~13 ns of that is timer overhead, so net ≈110 ns at p50. Be ready to say
  what is excluded: no NIC receive, no wire-to-book, KVM guest not bare metal,
  pre-market flow only.
- **"Is that good?" / "How does it compare?"** — there is no published
  per-stage number to compare against, and the public tick-to-trade figures
  measure a different, much larger path. Say that, say what the number
  measures, say no optimisation pass has been done. Then pivot to what the
  measurement *found* (§4, §5) — that is the stronger material, and it is
  material nobody can dispute because the evidence is in the repo.
- **"Walk me through what happens on an `E` message."** — ref-index lookup
  → recover order (price/side from the stored `RestingOrder`) → find level
  → reduce qty → if zero, unlink from FIFO, free slot, clear bitmap bit if
  level emptied (cascading up the tiers), erase from ref index.
- **"Why does replace lose time priority?"** — ITCH `U` is delete-old +
  add-new; the new ref goes to the *tail* of its level's FIFO. That's real
  exchange semantics, not an implementation shortcut.
- **"How do you know it's correct?"** — layered: unit tests pin invariants
  (bitmap cascade, probe chains); a **differential fuzz** checks `RefIndex`
  against `std::unordered_map` after every one of 20 000 operations; a full
  replay asserts no crossed book and `not_found == 0`; and an **independent
  Python scan of the raw bytes** provides ground truth the C++ can be checked
  against. The last one is what actually caught the erase bug — worth saying,
  because "I built a second implementation to disagree with the first" is a
  strong answer.
- **"Does ITCH give you snapshots?"** — No. It's pure incremental from session
  start. Mid-session joins use **GLIMPSE**, a separate TCP snapshot service that
  returns the book plus the sequence number it corresponds to; MoldUDP64
  retransmission covers small gaps. And you never *repair* a diverged book — you
  stop quoting the symbol, re-snapshot, and resume.
- **A tradeoff you *chose and could reverse*** — fixed window vs. sliding;
  bitmap vs. cached touch; LIFO free list; backward-shift vs. tombstones;
  `Event` type or not. Being able to argue *both sides* is the signal.
- **A bug you found and how** — have both §4 and §5 ready. The reasoning process
  matters more than the bug.

## Cross-references
- `docs/order-book.md` — the committed book design in detail.
- `docs/feed-handler.md` — ITCH decode / message routing.
- `docs/latency-harness.md` — how latency is measured (the number above).
- `docs/execution-simulator.md` — queue-position modeling (why L3 matters).
