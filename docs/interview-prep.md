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

**The `ref == 0` subtlety (great story):** empty slots are marked
`ref == 0`, so 0 is an illegal key. **Nasdaq's Trade (P) message carries
ref = 0** (hidden-order executions, zeroed for anonymity) — but `P` is a
*trade print, not an order-book event*, so it must never reach the book.
`insert` **asserts `ref != 0`** as a tripwire that catches a feed-handler
routing bug at the source instead of as silent corruption later. This is
the kind of protocol-detail-meets-implementation-invariant that shows real
engineering.

---

## 5. Dense ring-array of price levels, not `std::map<price, qty>`

**Decision:** Per side, a contiguous array of `Level` structs indexed by
tick offset from a moving `base_price_`, as a **circular buffer**. Price is
already an integer tick, so price→level is array indexing, not a tree
lookup.

**Why:**
- `std::map` (red-black tree) is the "correct but slow" baseline: O(log n)
  per update, **a pointer-chase per node** (terrible cache behavior), and a
  heap allocation per new level. Fine for a lot of software, not for a
  latency-critical hot path.
- An array indexed by tick is **O(1) update and O(1) best** (with the
  bitmap, below), and **cache-friendly** — levels near the touch are
  contiguous. Since `price_t` is an integer tick, the index is just
  `price - base_price_`.

**Why a *ring* (circular buffer), not a flat array:** prices drift over the
day. A fixed window that rejects out-of-range prices is a toy. A flat array
that you shift when price moves costs an O(window) memcpy per re-center.
The ring makes **re-centering a cheap `ring_origin_` bump** — advance the
origin, clear only the vacated slots (O(distance moved), and distance is a
tick or two per event). This is the production-grade version of the sliding
window.

**Tradeoff:** the ring adds indexing complexity (`(offset + ring_origin_)
& mask_`) and the wrap means **physical slot order ≠ price order** — which
is exactly why the touch bitmap is indexed by *offset*, not ring slot (see
below). Far-from-touch orders don't get a level at all (kept only in the
pool + ref index) — nobody reads depth thousands of ticks out on the hot
path, and it's counted (`far_orders_`).

**When `std::map` would win:** if you genuinely need full, unbounded depth
with prices anywhere, or the book is so sparse the array wastes memory. For
a near-touch-focused HFT book, the array/bitmap wins decisively.

---

## 6. Hierarchical bitmap for O(1) best-bid/ask, not a scan or a scalar

**Decision:** Per side, a two-tier bitset marking which levels are
non-empty: 64 detail words (`bits_[64]` = 4096 bits, one per level) + a
64-bit summary word (bit *j* = "detail word *j* is non-zero"). Best
bid/ask = a couple of `clz`/`ctz` (count-leading/trailing-zeros)
instructions.

**Why this is the crux question — "how do you find the best price?":**
- **Naive scalar cache** (`best_bid_idx_`) alone: O(1) to *read*, but when
  the touch level empties on a delete you must **find the next-best**,
  which is an O(window) scan downward. Deletes at the touch are extremely
  common → that scan is the hot path. Not acceptable.
- **Scan the level array** every query: O(window). Worse.
- **Ordered tree** (`std::map::begin()`): O(1) read but O(log n) erase and
  bad cache behavior — the thing the array design exists to avoid.
- **Bitmap:** "find highest/lowest non-empty level" = "find highest/lowest
  set bit among 4096." The summary tier means you jump straight to the
  right 64-bit word (one `clz`) instead of scanning up to 64 words, then
  find the bit within it (one more `clz`). **O(1), a handful of
  instructions, no data-dependent loop.** Composes perfectly with the dense
  array because it's just a parallel bit-index over the same level indices.

**Key invariant to state:** "summary bit *j* set ⟺ `bits_[j]` != 0." Adds
set a detail bit + (unconditionally) its summary bit; deletes clear the
detail bit + the summary bit *only if the whole word hit zero* (the
asymmetry: adding always makes a word non-zero; removing only *sometimes*
empties it). A bug here shows up as a wrong best price — so it's the first
thing tests should pin.

**Why indexed by logical offset, not ring slot:** `clz`/`ctz` find the
highest/lowest *bit*, which must correspond to highest/lowest *price*.
Offsets are monotonic in price; ring slots wrap (physical order ≠ price
order after a re-center). So the bitmap uses `offset = price - base_price_`,
while the level array uses the ring slot — both derived from the same
offset. This offset-vs-ring split is a favorite "did you actually think
about the wrap?" follow-up.

**Why 4096 = 64×64 exactly:** two tiers cover 64×64 bits. More levels →
add a third tier. This is the standard hierarchical-bitmap trick (also seen
in schedulers, allocators, `find-first-set` structures).

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

**When you'd add an `Event` type:** if multiple independent consumers need
the same normalized stream (book + tape + recorder), or you want to
decouple decode from apply across a queue/thread boundary. Then the
normalization pays for itself. Know this tradeoff — it's a "when does
abstraction cost too much" question.

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

## Things to have crisp for the interview

- **"What's your p99 latency per message?"** — HFT interviews *start* here.
  You must have a measured number (see latency-harness). Know where the
  time goes: the ref-index lookup on ~60% of messages is the prime suspect.
- **"Walk me through what happens on an `E` message."** — ref-index lookup
  → recover order (price/side from the stored `RestingOrder`) → find level
  → reduce qty → if zero, unlink from FIFO, free slot, clear bitmap bit if
  level emptied, erase from ref index.
- **"Why does replace lose time priority?"** — ITCH `U` is delete-old +
  add-new; the new ref goes to the *tail* of its level's FIFO. That's real
  exchange semantics, not an implementation shortcut.
- **"How do you know it's correct?"** — replay a full session, assert the
  book is empty after the end-of-day `S` message. (Also: no crossed book.)
- **A tradeoff you *chose and could reverse*** — ring vs. hash-map levels;
  bitmap vs. scan; LIFO free list; open vs. closed addressing; `Event` type
  or not. Being able to argue *both sides* is the signal.

## Cross-references
- `docs/order-book.md` — the committed book design in detail.
- `docs/feed-handler.md` — ITCH decode / message routing.
- `docs/latency-harness.md` — how latency is measured (the number above).
- `docs/execution-simulator.md` — queue-position modeling (why L3 matters).
