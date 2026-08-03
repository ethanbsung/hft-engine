# Module: Order Book

**File(s):** `include/hft/orderbook.hpp`, `src/orderbook.cpp`
**Phase:** 1 · **Status:** 🟩 implemented (apply path + queries + fixed-window levels + three-tier touch bitmap; benchmarked, see `docs/benchmarks.md`)

## Responsibility
Maintain the current state of the limit order book for each symbol, and
answer the queries the strategy needs — primarily **top of book** (best bid
/ best ask, sizes) and level lookups.

The book is **driven directly by the decoder**, not by a normalized event
stream: the decoder's `switch` calls `add_order` / `delete_order` /
`execute_order` / `cancel_order` / `replace_order` (feed-handler.md §1).
There is no `Event` type between them. The book still sees no wire bytes —
the decoder has already turned offsets into typed arguments — but the seam
is a set of methods, not a struct.

## Feed semantics (drives the data structure)
**Nasdaq ITCH 5.0 is order-by-order (L3), not price-level (L2).** The
book is driven by individual order lifecycle messages, keyed by
**order reference number** (`ref`):

| Msg | Effect |
|---|---|
| `A` / `F` | add order `ref` at (symbol, side, price, shares) |
| `E` | order `ref` executed N shares — **no price in the message** |
| `C` | order `ref` executed N shares at a *different* price |
| `X` | order `ref` cancelled N shares (partial) |
| `D` | order `ref` deleted entirely |
| `U` | delete old `ref`, add **new** `ref` (price/qty change) |

Consequences that shape everything below:
- You **must** keep `order_ref → pool slot`. `E`/`X`/`D` identify the order
  *only* by `ref`; you cannot find the price level without this map.
- Price levels hold **aggregate qty**, derived by applying each order
  event to its level. A level disappears when its aggregate hits zero.
- `U` is **not** an in-place update: old ref dies, new ref is born, and
  the new order **loses time priority**.

### `ref == 0` is reserved — never route Trade (P) into the book
Only `A`/`F` (and `U`'s *new* ref) introduce a ref; all are **live,
day-unique, non-zero** tracking IDs, assigned roughly monotonically.

**Message `P` (Trade Non-Cross) carries Order Reference Number = 0** by
spec — it is the execution broadcast for a *hidden* (non-displayed) order,
zeroed to preserve anonymity (confirmed for all `P` messages since
2010-12-06). `P` is a **trade print, not an order-lifecycle event**: it
never creates, modifies, or deletes a resting order, so it **must not be
routed to `OrderBook`** — it belongs to the trade tape / analytics path,
not `add_order`/`execute_order`/etc.

`RefIndex` uses **0 as its empty-slot sentinel**, so 0 is an illegal key.
This is safe *because* no book event ever carries ref 0. `RefIndex::insert`
**asserts `ref != 0`** (debug builds) as a tripwire: if it ever fires, a
`P` (or a mis-parsed message) is being wrongly fed into the book — a
feed-handler routing bug, caught at the source instead of as silent
corruption later.

## Data structure — the committed design

This book is built to the HFT-grade design from the start (per the repo's
"build the production-grade version the first time" bar), **not** a
`std::map`/`unordered_map` baseline to be optimized later. Four
cooperating structures:

### (a) Order pool — `std::vector<RestingOrder> pool_`
A preallocated arena of `RestingOrder` records, sized once at construction
(`pool_capacity`). One slot = one live order. **No per-order heap
allocation on the hot path** — that is the entire point. A LIFO free list
threaded through each free slot's `next_idx` tracks unused slots;
`alloc_slot` pops the head, `free_slot` pushes it back. LIFO because the
most-recently-freed slot is the most cache-warm, and slots are
interchangeable (no ordering requirement). `free_head_` names the top of
the free list; `kNullIdx` means exhausted.

`RestingOrder` stores `price` and `side` **denormalized** onto the order
itself, alongside `shares`, the intrusive `prev_idx`/`next_idx` links, `ref`,
and an `is_far` flag. This is deliberate: `E`/`X`/`D`/`U` arrive with only a
`ref`, so after the ref→slot lookup we must recover which level to touch —
`price` and `side` on the order give us that in O(1) without a back-pointer.

`is_far` records, at insert time, whether the order was placed in a level
(near) or parked outside the window (far). Teardown **reads** this bit rather
than re-deriving far/near from `index_of(price)`, so insertion and teardown are
symmetric by construction: whatever the add did, the remove undoes.

With a fixed `base_price_` the two would agree anyway — the window never moves,
so `index_of` gives the same answer at teardown as at insert. The flag was
load-bearing under the old sliding-window design, where a re-centre could slide
a far order inside the window (or vice versa) and recomputing at teardown would
take the near branch for an order never linked into a level, corrupting that
level's aggregate. It is kept because storing insertion-time truth is the more
robust contract: it stays correct if the window ever becomes movable again, and
it costs one bit in a struct that has padding to spare.

**Two distinct sentinels.** `kNullIdx` (`UINT32_MAX`) is the *null pool link*
— an empty free list, an unlinked list end, an empty level's `head_idx`. It
is also what `RefIndex::find` returns for a miss. `kNoLevel` (also
`UINT32_MAX`, but a separate named constant for intent) is what `index_of`
returns when a price is **outside the price window** — the far-order signal.
`kInvalidPrice` (`INT64_MIN`) is the "no such side" return from
`best_bid`/`best_ask` on an empty book.

### (b) Ref index — `RefIndex ref_index_`
Open-addressed hash map `order_ref → pool slot`. This lookup fires on
**every** `E`/`C`/`X`/`D`/`U` — ~60% of all messages — so it is a
first-class performance concern (hash function, load factor, open- vs.
closed-addressing all show up here). Sized to a power of two, comfortably
larger than `pool_capacity` (currently `pool_capacity * 2`, ~50% max load)
so open addressing stays healthy.

### (c) Price levels — dense fixed-window array
Per side, a contiguous `std::vector<Level>` (`bid_levels_`, `ask_levels_`)
of `window` entries (`window` a power of two → `mask_ = window - 1`),
indexed by **tick offset from a fixed `base_price_`**:

```
offset = price - base_price_          // monotonic in price; also the bitmap index
```

`base_price_` is supplied at construction and **never moves**. `BookSet`
computes it as `reference_price - window/2` from a per-symbol reference —
hardcoded per symbol for the replay fixture, sourced from the previous
session's close in production. `window` is 32 768 ticks, so the book spans
±\$163.84 around the reference (≈±27% on a \$600 stock, comfortably outside
LULD halt bands).

Array indexing + cache locality is the reason for the array over a tree.
Each `Level` holds `total_qty`, `order_count`, and `head_idx`/`tail_idx`
into the pool — the level does **not** store orders, it points at the head
and tail of a per-level intrusive doubly-linked list of pool slots. New
orders append at the **tail** → **price-time (FIFO) priority** within a
level, O(1). Deletes unlink from the middle in O(1) via `prev_idx`/
`next_idx`.

**Why fixed and not a sliding ring — this design was reversed.** The book
originally used a circular buffer whose `ring_origin_` advanced as price
drifted, re-centring whenever an incoming price landed outside the window. The
trigger was the defect: one deep resting order dragged the whole window onto
itself, evicting the near-mid book. It cost ~14 000 evicted orders and the
entire latency tail per replay — the README has the write-up, `benchmarks.md`
§4 the measured effect.

For penny-tick equities the memory a sliding window saves does not justify the
complexity: 32 768 levels × 24 B × 2 sides is ≈1.6 MB per symbol. The cases
where sliding *is* the right call, and what its trigger must key off instead,
are in `design-decisions.md` §5.

**Far orders.** Orders priced outside the window still live in `pool_` +
`ref_index_` (so `E`/`X`/`D`/`U` on them still resolve) but are **not** placed
in a level and carry no touch bit. In the real feed these are ITCH's
\$199,999.99 "no price" sentinel, penny stubs, and genuinely deep passive
orders — none of which a market maker quotes against. Counted (`far_orders_`)
so a mis-sized window is observable. `remove_at_slot` recognises a far order by
its stored `is_far` flag and tears it down with just a slot-free + ref-erase,
no level bookkeeping.

### (d) Touch bitmap — hierarchical non-empty-level bitset
Per side, a **three-tier** bitmap marking which levels are non-empty, so
`best_bid`/`best_ask` and the touch-refresh on delete are **O(1)**, not an
O(window) scan:

- `bid_bits_[512]` / `ask_bits_[512]`: 512 words × 64 bits = 32 768 detail
  bits, one per level. Bit set ⇔ that level has ≥1 resting order.
- `bid_mid_[8]` / `ask_mid_[8]`: 8 words × 64 bits = 512 bits; bit *j* set ⇔
  detail word *j* is non-zero.
- `bid_summary_` / `ask_summary_`: one `uint64_t`; bit *k* set ⇔ mid word *k*
  is non-zero. 8 bits used of 64.

Each tier narrows the search 64-fold, so *n* tiers reach 64ⁿ levels: two tiers
cap `window` at 4 096 (only ±\$20.48 at penny ticks — the original sizing, and
too narrow, which is what forced the sliding window in the first place); three
tiers reach 262 144, so the 32 768 window fits with room to spare. The
constructor asserts `window <= 32768` to keep `offset >> 6` inside
`bits_[512]`.

The bitmap is indexed by the same `offset` as the level array, so bit order ==
price order. Finding the best level is three `clz`/`ctz` instructions:
- **best bid** (highest set bit): top set bit of `summary` → top set bit of
  that mid word → top set bit of that detail word.
- **best ask** (lowest set bit): the same walk with `ctz` at each tier.

The bitmap is the **source of truth** for the touch — there are no cached
`best_*_idx_` scalars to keep consistent. `best_bid`/`best_ask` return
`kInvalidPrice` when their side's summary word is zero (empty book).

**The set/clear asymmetry is the crux of the structure.** `add_order` sets the
detail bit when a level goes empty→non-empty and sets the mid and summary bits
above it **unconditionally** — adding always makes a word non-zero.
`remove_at_slot` clears the detail bit, then clears the mid bit **only if that
whole detail word hit zero**, then the summary bit **only if that whole mid word
hit zero** — removing only *sometimes* empties a word. Getting that nesting
wrong either reports a phantom price (bit left set) or loses a live level (bit
cleared too eagerly). `tests/test_orderbook.cpp` pins both directions at every
tier, including the "sibling still occupied in the same word" cases.

## Observability counters
The book keeps a handful of `uint64_t` counters, cheap to bump and meant to make
invariant violations and mis-sizing *observable* rather than silent:

| Counter | Kind | Bumped when |
|---|---|---|
| `not_found_` | monotone | an `E`/`X`/`D`/`U` names a `ref` not in the index |
| `pool_full_drops_` | monotone | `alloc_slot` fails (pool exhausted) → order dropped |
| `far_orders_` | **gauge** | +1 on an add outside the window, −1 when that order is torn down — so it reads *live* far orders, not a running total |

Only `far_orders_` decrements; the other two are cumulative for the life of the
book. `not_found()` and `far_orders()` are public; `pool_full_drops_` currently
has no accessor, so the sizing signal described below is not actually readable
from outside the class.

`not_found_` is the important correctness tripwire: under a clean, in-order
replay it should stay **zero**, and on the 500 MB fixture it does. A nonzero
value means a feed gap (missed the `A`/`F` that introduced the ref), a routing
bug (e.g. a `P` trade wrongly fed into the book), or — as it turned out — a bug
in the book itself. It earned its keep: a nonzero `not_found_` was the thread
that led to the `RefIndex::erase` defect described in the README, which was
silently orphaning entries in hash collision chains and leaving books crossed.

In a live system this counter would not merely count. Crossing a threshold
would trip a per-symbol health flag that **stops quoting that symbol** and
triggers a snapshot recovery — ITCH carries no in-feed snapshot, so recovery
means a GLIMPSE request (a separate TCP service returning the full book plus
the sequence number it corresponds to) and replaying forward from there. You
never repair a diverged book in place; you rebuild it.

`pool_full_drops_` staying zero and `far_orders_` staying small are the signals
that `pool_capacity` and `window` are sized correctly for the symbol.

## Indexing — one index space
Levels and the touch bitmap share a single index: `offset = price -
base_price_`, monotonic in price and fixed for the life of the book. Highest
set bit = highest price = best bid; lowest set bit = best ask.

That single index space is the main structural payoff of the fixed window: the
ring design needed two (a physical ring slot for levels, a logical offset for
the bitmap, kept in agreement by hand), and mismatches between them were a live
source of phantom-price bugs.

## Multi-symbol
ITCH is a **single stream carrying all ~8,000 Nasdaq symbols**, not one
connection per symbol. Every message has a `stock_locate` field (a
`uint16` id assigned in the `R` StockDirectory messages at start of day)
— that is a **dense integer index**, exactly what `SymbolId` is for.
Prefer `stock_locate` over parsing the 8-char symbol on the hot path.

Practically: filter to a handful of symbols, or accept a book per locate.
Don't build 8,000 books unless you mean to. This `OrderBook` is **one book
per symbol** (`symbol_` set at construction).

## Snapshot / gap integration
- ITCH has **no snapshot message**. The book is built from start-of-day
  and replayed forward; recovery is by **retransmit of missed packets**
  (MoldUDP64), not by re-snapshotting.
- A gap signal → book is invalid; strategy stops quoting until the
  retransmit is applied (coordinate via the wiring layer).
- `S` (SystemEvent) marks start/end of day and session boundaries — use
  it to know when the book should be empty.

## Interface
One public mutator per ITCH message effect — these are exactly what the
decoder's `switch` calls (feed-handler.md §1). Arguments are the wire fields
of that message, already byte-swapped:

```cpp
class OrderBook {
public:
    OrderBook(SymbolId symbol, price_t base_price,
              std::size_t window, std::size_t pool_capacity);

    // mutations, called by the decoder:
    void add_order(order_ref_t ref, Side side, price_t price,
                   qty_t shares) noexcept;                       // A / F
    void execute_order(order_ref_t ref, qty_t shares) noexcept;  // E / C
    void cancel_order(order_ref_t ref, qty_t shares) noexcept;   // X
    void delete_order(order_ref_t ref) noexcept;                 // D
    void replace_order(order_ref_t old_ref, order_ref_t new_ref,
                       price_t price, qty_t shares) noexcept;     // U

    // hot query:
    price_t best_bid() const noexcept;   // kInvalidPrice if no bids
    price_t best_ask() const noexcept;   // kInvalidPrice if no asks
    qty_t   qty_at(Side side, price_t price) const noexcept;
};
```

Note what `execute_order` / `cancel_order` / `delete_order` /
`replace_order` **don't** take: no symbol, no side, no price. ITCH
identifies the order by `ref` alone and expects you to recover the rest via
`ref_index_` → pool slot → the order's stored `side`/`price`. That is why
`replace_order` reads `old_ref`'s side from its `RestingOrder` *before*
deleting it, then re-adds `new_ref` at the new price/qty (which **loses time
priority** — it is appended at the tail of the new level).

**Shared reduce path.** `execute_order` (E) and `cancel_order` (X) are
identical to the book: both reduce the resting order's shares by `shares`.
They therefore both delegate to a private `reduce(ref, shares)` which decrements
the order (and its level's `total_qty`) if `shares` is partial, or removes the
order entirely if `shares` meets or exceeds what remains. The distinction
between an execution and a cancel (fill vs. pulled liquidity) matters to the
trade tape and analytics, not to book state, so it is not preserved here.

**`remove_at_slot(uint32_t)` — the shared teardown.** `delete_order`, the
`reduce` full-removal branch, and `replace_order` (via `delete_order`) all
converge on `remove_at_slot`: unlink the order from its level's intrusive
list, decrement `order_count`/`total_qty`, clear the touch bit if the level
went empty, free the pool slot, and erase the ref from `ref_index_`. The
far-order case (order lives in the pool/ref-index but not in a level, flagged
by `is_far`) is handled first with just a slot free + ref erase.

## Latency notes
`add_order` / the reduce path / `best_bid`/`best_ask` are the hottest
read/write. Under replay there is no network, so **book update cost is a
first-order term in the only latency number you have** — not noise against
a network floor.

The `order_ref` hash lookup fires on ~60% of messages (`E`/`C`/`X`/`D`/`U`)
and is a prime candidate for measurement: hash function, load factor,
and open- vs. closed-addressing all show up here.

**Benchmark apply + best-of-book on Linux, pinned.** The design is already
the array/bitmap version; measurement is to *tune* it (band width, load
factor, layout), not to decide whether to move off a `std::map` baseline.

## Done checklist
- [x] Order pool + LIFO free list (`alloc_slot` / `free_slot`)
- [x] `ref_index_` (open-addressed `ref → slot`, backward-shift erase)
- [x] `index_of` (fixed-window offset, far-order detection)
- [x] `add_order` (A/F): alloc, fill, tail-link (FIFO), ref-index insert,
      set touch bits (all three tiers); far-order path
- [x] `delete_order` (D) → `remove_at_slot`: unlink, level decrement, free
      slot, ref-index erase, cascading touch-bit clear on empty
- [x] `execute_order` (E/C), `cancel_order` (X) → shared `reduce`: reduce
      shares; remove when it hits zero
- [x] `replace_order` (U = read old side → delete → add new ref, loses time
      priority)
- [x] `best_bid` / `best_ask` (three-tier bitmap `clz`/`ctz`), `qty_at`
- [x] Fixed window with per-symbol reference price (replaced sliding ring;
      see `docs/benchmarks.md` §4)
- [x] Tests: partial execute, partial cancel, delete-empties-level, `U`
      re-add, far-order round-trip (above/below window, execute-by-ref,
      slot reuse), window boundaries, three-tier bitmap set/clear cascade
      at every tier (`tests/test_orderbook.cpp`)
- [x] Tests: `RefIndex` collision-chain integrity + differential fuzz against
      `std::unordered_map` (`tests/test_ref_index.cpp`)
- [x] Cross-check: full 500 MB replay leaves all 7 books uncrossed with
      `not_found_ == 0`, matching an independent scan of the raw ITCH bytes
- [x] Benchmark apply + best query (Linux, pinned) — re-run post-fix; results
      in `docs/benchmarks.md`
- [ ] Assert book empty after end-of-day `S` message (needs a full-session
      fixture; the current one ends at 09:30 ET)
- [ ] Staleness signal: promote `not_found_` from counter to per-symbol health
      flag that halts quoting and triggers snapshot recovery
- [ ] (Later) per-level queue-position view for the execution simulator
