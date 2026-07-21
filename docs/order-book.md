# Module: Order Book

**File(s):** `include/hft/orderbook.hpp`, `src/orderbook.cpp`
**Phase:** 1 · **Status:** 🟩 implemented (apply path + queries + re-centering complete; benchmarks pending)

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
than re-deriving far/near from `index_of(price)` against the *current* base:
an order's far/near status is fixed when it is added, but a later re-center
slides `base_price_`/`ring_origin_`, so a far order can drift inside the
window (or vice versa) and the two frames disagree. Recomputing at teardown
would misclassify — take the near branch for an order never linked into a
level, corrupting that level's aggregate and unlinking whatever rested there.
Storing insertion-time truth and reading it back makes teardown symmetric
with insertion and immune to base motion.

**Two distinct sentinels.** `kNullIdx` (`UINT32_MAX`) is the *null pool link*
— an empty free list, an unlinked list end, an empty level's `head_idx`. It
is also what `RefIndex::find` returns for a miss. `kNoLevel` (also
`UINT32_MAX`, but a separate named constant for intent) is what `index_of`
returns when a price is **outside the ring window** — the far-order signal.
`kInvalidPrice` (`INT64_MIN`) is the "no such side" return from
`best_bid`/`best_ask` on an empty book.

### (b) Ref index — `RefIndex ref_index_`
Open-addressed hash map `order_ref → pool slot`. This lookup fires on
**every** `E`/`C`/`X`/`D`/`U` — ~60% of all messages — so it is a
first-class performance concern (hash function, load factor, open- vs.
closed-addressing all show up here). Sized to a power of two, comfortably
larger than `pool_capacity` (currently `pool_capacity * 2`, ~50% max load)
so open addressing stays healthy.

### (c) Price levels — dense ring array near the touch
Per side, a contiguous `std::vector<Level>` (`bid_levels_`, `ask_levels_`)
of `window` entries (`window` a power of two → `mask_ = window - 1`),
indexed by **tick offset from `base_price_`** through a **circular
buffer**:

```
offset = price - base_price_          // logical, monotonic in price
slot   = (offset + ring_origin_) & mask_   // physical ring slot
```

Array indexing + cache locality is the reason for the array over a tree.
Each `Level` holds `total_qty`, `order_count`, and `head_idx`/`tail_idx`
into the pool — the level does **not** store orders, it points at the head
and tail of a per-level intrusive doubly-linked list of pool slots. New
orders append at the **tail** → **price-time (FIFO) priority** within a
level, O(1). Deletes unlink from the middle in O(1) via `prev_idx`/
`next_idx`.

**Re-centering (sliding window).** Prices drift; the array covers a band
around the inside. `add_order` triggers `recenter` when an incoming price
falls outside the window (`index_of` returns `kNoLevel`) *but* is still
within a window's width of the current center — i.e. a genuine drift, not a
wild outlier. (A price beyond that guard is treated as a far order, below,
rather than dragging the whole window to it.) `recenter(new_center)` rebases
so the window is centered on `new_center` (`new_base = new_center -
window/2`), then re-derives the level index.

`recenter` has two paths, chosen by how far the base moves (`delta`):

- **Partial slide** (`|delta| < window`): only the `|delta|` slots that
  scroll off the trailing edge are vacated. Each is `evict_level`'d (its
  resting orders freed from the pool and erased from `ref_index_`, counted
  in `evicted_`) and reset to an empty `Level{}`; `base_price_` and
  `ring_origin_` advance by `delta` (mod `window`) so surviving levels keep
  their price↔slot mapping for free — no array shift. Cost is O(|delta|),
  and `|delta|` is a tick or two per event, not thousands.
- **Full clear** (`|delta| ≥ window`): the whole old window has scrolled
  out of view, so every level is evicted, all levels/bitmaps are zeroed,
  and `ring_origin_` resets to 0. O(window), but rare.

After a slide the touch bitmap is recomputed by `rebuild_bitmap`, an
O(window) rescan that re-derives every set bit from the non-empty levels.
(Rebuilding wholesale rather than incrementally clearing the vacated bits is
the simpler correct choice; it is off the common hot path since re-centers
are infrequent, and is a candidate for incremental clearing if the
benchmark says it matters.) Re-centering count is tracked (`recenters_`).

**Far orders.** Orders whose price is outside the near-touch band (and
outside the re-center guard) still live in `pool_` + `ref_index_` (so
`E`/`X`/`D`/`U` on them still work) but are **not** placed in a level and
carry no touch bit. Nobody reads depth thousands of ticks off the touch on
the hot path. Counted (`far_orders_`) so a mis-sized band is observable.
`remove_at_slot` recognizes a far order by its stored `is_far` flag (set at
insert time, not recomputed from the current window) and tears it down with
just a slot-free + ref-erase, no level bookkeeping.

### (d) Touch bitmap — hierarchical non-empty-level bitset
Per side, a two-tier bitmap marking which levels are non-empty, so
`best_bid`/`best_ask` and the touch-refresh on delete are **O(1)**, not an
O(window) scan:

- `bid_bits_[64]` / `ask_bits_[64]`: 64 words × 64 bits = 4096 detail
  bits, one per level. Bit set ⇔ that level has ≥1 resting order.
- `bid_summary_` / `ask_summary_`: one `uint64_t`; bit *j* set ⇔ detail
  word *j* is non-zero. 64 summary bits cover all 64 detail words.

The fixed two-tier size caps `window` at **4096** (`offset >> 6` must land
in `[0, 64)`). This is comfortably larger than any near-touch band worth
keeping dense; a larger band belongs in the far-order bucket, not a wider
bitmap.

The bitmap is indexed by **logical offset**, not ring slot, so bit order
== price order (see "Ring + bitmap indexing" below). Finding the best
level is two `clz`/`ctz` instructions:
- **best bid** (highest set bit): top set bit of `summary` → top set bit
  of that detail word.
- **best ask** (lowest set bit): bottom set bit of `summary` → bottom set
  bit of that detail word.

The bitmap is the **source of truth** for the touch — there are no cached
`best_*_idx_` scalars to keep consistent. `add_order` sets a bit when a
level goes empty→non-empty; `remove_at_slot` clears a bit when a level goes
non-empty→empty (and clears the summary bit when its detail word hits
zero). `best_bid`/`best_ask` return `kInvalidPrice` when their side's
summary word is zero (empty book).

## Observability counters
The book keeps a handful of monotone `uint64_t` counters, cheap to bump and
meant to make invariant violations and mis-sizing *observable* rather than
silent:

| Counter | Bumped when |
|---|---|
| `pool_full_drops_` | `alloc_slot` fails (pool exhausted) → order dropped |
| `far_orders_` | an add lands outside the band (net live far orders) |
| `recenters_` | a `recenter` runs |
| `evicted_` | a resting order is dropped by a re-center's eviction |
| `not_found_` | an `E`/`X`/`D`/`U` names a `ref` not in the index |

`not_found_` is the important correctness tripwire: under a clean, in-order
replay it should stay **zero**. A nonzero value means either a feed gap
(missed the `A`/`F` that introduced the ref) or a routing bug (e.g. a `P`
trade wrongly fed into the book). `pool_full_drops_` and `far_orders_` /
`evicted_` staying near zero is the signal that `pool_capacity` and `window`
are sized correctly for the symbol.

## Ring + bitmap indexing — why the two use different indices
The **levels** are a ring (physical slot = `(offset + ring_origin_) &
mask_`) so re-centering is a cheap origin bump instead of an array shift.
But after a recenter the ring wraps, so **physical slot order ≠ price
order** across the wrap — you cannot `clz` over physical slots to find the
best *price*. The **bitmap** is therefore indexed by **logical offset**
(monotonic in price): highest set bit = highest price = best bid, lowest
set bit = best ask, regardless of where the ring origin currently sits.
Level lookup uses the ring slot; touch lookup uses the offset bit. Both
derive from the same `offset`.

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
- [x] `ref_index_` (open-addressed `ref → slot`)
- [x] `index_of` (ring offset→slot, far-order detection)
- [x] `add_order` (A/F): alloc, fill, tail-link (FIFO), ref-index insert,
      set touch bit; far-order path; recenter trigger
- [x] `delete_order` (D) → `remove_at_slot`: unlink, level decrement, free
      slot, ref-index erase, clear touch bit on empty
- [x] `execute_order` (E/C), `cancel_order` (X) → shared `reduce`: reduce
      shares; remove when it hits zero
- [x] `replace_order` (U = read old side → delete → add new ref, loses time
      priority)
- [x] `best_bid` / `best_ask` (bitmap `clz`/`ctz`), `qty_at`
- [x] `recenter`: partial slide + full-clear paths, eviction of scrolled-out
      orders, bitmap rebuild
- [x] Tests: partial execute, partial cancel, delete-empties-level, `U`
      re-add, far-order round-trip, recenter (in `tests/test_orderbook.cpp`)
- [ ] Cross-check: replay a full session, assert book empty after end-of-
      day `S` message
- [ ] Benchmark apply + best query (Linux, pinned)
- [ ] (Later) per-level queue-position view for the execution simulator
