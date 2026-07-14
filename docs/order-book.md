# Module: Order Book

**File(s):** `include/hft/order_book.hpp` (to create), maybe `src/order_book.cpp`
**Phase:** 1 · **Status:** ⬜ not started

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
- You **must** keep `order_ref → (symbol, side, price, qty)`. `E`/`X`/`D`
  identify the order *only* by `ref`; you cannot find the price level
  without this map.
- Price levels hold **aggregate qty**, derived by applying each order
  event to its level. A level disappears when its aggregate hits zero.
- `U` is **not** an in-place update: old ref dies, new ref is born, and
  the new order **loses time priority**.

## Data structure — the core design decision
Two structures, not one:

**(a) The order map: `order_ref → (side, price, qty)`.**
`ref` is a `uint64` assigned roughly monotonically per day, and the live
set is large (millions of adds/day, most deleted quickly). An
`unordered_map` is the correct baseline. This lookup is on the hot path
for *every* `E`/`C`/`X`/`D`/`U` — that's ~60% of all messages — so it is
a first-class performance concern, not plumbing.

**(b) Per side, a price → aggregate-qty map** that is:
- **ordered** (best bid = max price, best ask = min price), and
- **fast to update at an arbitrary price**, and
- **very fast to read the best level** (the hot query).

Options to weigh for (b) (write your choice + reasoning into the header):
1. **`std::map<price_t, qty_t>`** (RB-tree). Ordered, O(log n) update,
   O(1) `begin()` for best. Simplest correct baseline. Node-per-level =
   pointer chasing / poor cache behavior. Great **first** implementation.
2. **Sorted `std::vector` + binary search.** Cache-friendly reads, but
   O(n) mid-vector insert/erase. Good when the book is shallow near top.
3. **Array indexed by price tick** (a "book as a flat array", price is
   already an integer tick!). O(1) update and O(1) best *if* you track
   the best index. This is the HFT-favorite when the price range is
   bounded — and since `price_t` is integer ticks, it's natural here.
   Needs a min/max tick window and a way to advance best on deletion.

**Recommendation:** start with (1) for correctness + tests, then, as a
*measured* optimization, move the hot side toward (3). Keep the query
interface identical so the swap doesn't touch the strategy.

> **Time priority now matters.** With L2 you only needed aggregate qty per
> level. With L3 you know *which* orders sit at a level and **in what
> order they arrived** — so you can model **queue position**: how much
> size rests ahead of you at your price. That is the single biggest
> realism upgrade available to the execution simulator
> (see execution-simulator.md), and it is *only* possible because the feed
> is order-by-order. Consider a per-level FIFO of order refs.

## Multi-symbol
ITCH is a **single stream carrying all ~8,000 Nasdaq symbols**, not one
connection per symbol. Every message has a `stock_locate` field (a
`uint16` id assigned in the `R` StockDirectory messages at start of day)
— that is a **dense integer index**, exactly what `SymbolId` is for.
Prefer `stock_locate` over parsing the 8-char symbol on the hot path.

Practically: filter to a handful of symbols, or accept a book per locate.
Don't build 8,000 books unless you mean to.

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
    void add_order(order_ref_t ref, SymbolId sym, Side side,
                   price_t px, qty_t shares) noexcept;   // A / F
    void execute_order(order_ref_t ref, qty_t shares) noexcept;   // E / C
    void cancel_order(order_ref_t ref, qty_t shares) noexcept;    // X
    void delete_order(order_ref_t ref) noexcept;                  // D
    void replace_order(order_ref_t old_ref, order_ref_t new_ref,
                       price_t new_px, qty_t new_shares) noexcept; // U

    // hot query:
    price_t best_bid() const noexcept;
    price_t best_ask() const noexcept;
    qty_t   bid_size_at(price_t) const noexcept;
    // ... top-N view for the strategy
};
```

Note what `execute_order` / `cancel_order` / `delete_order` / `replace_order`
**don't** take: no symbol, no side, no price. ITCH identifies the order by
`ref` alone and expects you to recover the rest from the order map — that's
why the map is mandatory (below), and why `replace_order` looks up
`old_ref`'s symbol+side internally before re-adding (feed-handler.md §3.1).

One `OrderBook` per symbol, or a book keyed by `SymbolId` internally —
decide and document.

## Latency notes
`apply` and `best_bid/ask` are the hottest read/write. Note that under
replay there is no network, so **book update cost is a first-order term in
the only latency number you have** — not noise against a network floor.

The `order_ref` hash lookup fires on ~60% of messages (`E`/`C`/`X`/`D`/`U`)
and is a prime candidate for measurement: hash function, load factor,
and open- vs. closed-addressing all show up here.

**Benchmark apply + best-of-book; measure before choosing structure #3
over #1.**

## Done checklist
- [ ] `order_ref → (side, price, qty)` map
- [ ] Baseline level structure (`std::map`) — apply / remove-on-zero
- [ ] `add_order` (A/F), `execute_order` (E/C), `cancel_order` (X),
      `delete_order` (D), `replace_order` (U = lookup old → delete → add,
      new ref, loses time priority)
- [ ] `best_bid` / `best_ask` / top-of-book view
- [ ] Tests: build from a replayed prefix, partial execute, partial cancel,
      delete-empties-level, `U` re-add, crossed-book guard
- [ ] Cross-check: replay a full session, assert book is empty after the
      end-of-day `S` message
- [ ] Benchmark apply + best query (Linux, pinned)
- [ ] (Later) per-level FIFO of order refs → queue position
- [ ] (Later, measured) flat-array structure for the hot side
