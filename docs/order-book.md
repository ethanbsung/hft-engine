# Module: Order Book

**File(s):** `include/hft/orderbook.hpp`, `src/orderbook.cpp`
**Phase:** 1 · **Status:** 🟨 in progress

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
itself. This is deliberate: `E`/`X`/`D`/`U` arrive with only a `ref`, so
after the ref→slot lookup we must recover which level to touch — `price`
and `side` on the order give us that in O(1) without a back-pointer.

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
around the inside. When the touch nears the array edge, `recenter` slides
the window by advancing `base_price_`/`ring_origin_` and clearing only the
vacated slots — O(distance moved), and distance is tiny per event because
price moves a tick or two, not thousands. This is the production-grade
alternative to a fixed window that rejects out-of-range prices (toy).
Re-centering count is tracked (`recenters_`).

**Far orders.** Orders whose price is outside the near-touch band still
live in `pool_` + `ref_index_` (so `E`/`X`/`D`/`U` on them still work) but
are **not** placed in a level. Nobody reads depth thousands of ticks off
the touch on the hot path. Counted (`far_orders_`) so a mis-sized band is
observable.

### (d) Touch bitmap — hierarchical non-empty-level bitset
Per side, a two-tier bitmap marking which levels are non-empty, so
`best_bid`/`best_ask` and the touch-refresh on delete are **O(1)**, not an
O(window) scan:

- `bid_bits_[64]` / `ask_bits_[64]`: 64 words × 64 bits = 4096 detail
  bits, one per level. Bit set ⇔ that level has ≥1 resting order.
- `bid_summary_` / `ask_summary_`: one `uint64_t`; bit *j* set ⇔ detail
  word *j* is non-zero. 64 summary bits cover all 64 detail words.

The bitmap is indexed by **logical offset**, not ring slot, so bit order
== price order (see "Ring + bitmap indexing" below). Finding the best
level is two `clz`/`ctz` instructions:
- **best bid** (highest set bit): top set bit of `summary` → top set bit
  of that detail word.
- **best ask** (lowest set bit): bottom set bit of `summary` → bottom set
  bit of that detail word.

The bitmap is the **source of truth** for the touch — there are no cached
`best_*_idx_` scalars to keep consistent. `add_order` sets a bit when a
level goes empty→non-empty; the delete path clears a bit when a level goes
non-empty→empty (and clears the summary bit when its detail word hits
zero).

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

## Interface (contract sketch)
One method per ITCH message effect — these are exactly what the decoder's
`switch` calls (feed-handler.md §1). Arguments are the wire fields of that
message, already byte-swapped:

```cpp
class OrderBook {
public:
    // mutations, called by the decoder:
    void add_order(order_ref_t ref, Side side, price_t price,
                   qty_t shares) noexcept;                       // A / F
    void execute_order(order_ref_t ref, qty_t shares) noexcept;  // E / C
    void cancel_order(order_ref_t ref, qty_t shares) noexcept;   // X
    void delete_order(order_ref_t ref) noexcept;                 // D
    void replace_order(order_ref_t old_ref, order_ref_t new_ref,
                       price_t price, qty_t shares) noexcept;     // U

    // hot query:
    price_t best_bid() const noexcept;
    price_t best_ask() const noexcept;
    qty_t   qty_at(Side side, price_t price) const noexcept;
};
```

Note what `execute_order` / `cancel_order` / `delete_order` /
`replace_order` **don't** take: no symbol, no side, no price. ITCH
identifies the order by `ref` alone and expects you to recover the rest via
`ref_index_` → pool slot → the order's stored `side`/`price`. That is why
`replace_order` reads `old_ref`'s side (and price) from its `RestingOrder`
*before* deleting it, then re-adds `new_ref` (which **loses time
priority**).

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
      set touch bit; far-order path done. (recenter trigger still TODO)
- [ ] `delete_order` (D): unlink, level decrement, free slot, ref-index
      erase, clear touch bit on empty
- [ ] `execute_order` (E/C), `cancel_order` (X): reduce shares; remove when
      it hits zero
- [ ] `replace_order` (U = read old side/price → delete → add new ref,
      loses time priority)
- [ ] `best_bid` / `best_ask` (bitmap `clz`/`ctz`), `qty_at`
- [ ] `recenter` slow path (slide window, clear vacated slots + bits)
- [ ] Tests: partial execute, partial cancel, delete-empties-level,
      `U` re-add, crossed-book guard, far-order round-trip, recenter
- [ ] Cross-check: replay a full session, assert book empty after end-of-
      day `S` message
- [ ] Benchmark apply + best query (Linux, pinned)
- [ ] (Later) per-level queue-position view for the execution simulator
