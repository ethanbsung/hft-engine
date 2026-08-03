# Module: Types & SymbolTable

**File(s):** `include/hft/types.hpp`
**Phase:** 1 · **Status:** 🟩 primitives in use throughout the engine.
`SymbolTable` is defined but **not yet wired** — symbol resolution currently
goes through `BookSet`'s `stock_locate` array (see below).

## Responsibility
Define the vocabulary the entire system speaks: time, price, quantity,
side, and the exchange order reference.

## There is no normalized event type
An earlier `BookDelta` struct carried `new_qty` ("absolute resting qty at a
price level") — a **price-level (L2)** concept. Nasdaq ITCH is
**order-by-order (L3)**: `AddOrder` carries an order reference; `OrderExecuted`
says "order `ref` traded N shares" and never mentions a price. `BookDelta` could
not express that, and was deleted rather than reshaped.

There is no replacement event struct (feed-handler.md §1). With one thread and
one venue, the decoder's `switch` calls book methods (`add_order`,
`delete_order`, …) directly, so `types.hpp` defines the *primitives* those
signatures are built from, not a tagged union. Full rationale in
`design-decisions.md` §8.

## Design decisions
- **`price_t = int64_t` integer ticks.** Exact, fast, cache-able as an
  array index. Avoids the exact-equality / fragile-keying problems a
  `double` price would create on book levels. **ITCH prices arrive as
  `uint32` with 4 implied decimals** (`198400` = `$19.8400`) — already
  integer on the wire, so there is no string→number conversion at all.
- **`qty_t = int64_t`** in the smallest tradeable unit (ITCH: shares).
- **`nanos_t = int64_t` monotonic ns**, read via `platform::now_ns()`.
  Note ITCH's own timestamp is **6-byte big-endian ns since midnight ET**
  — a wall-clock-ish venue timestamp, *not* comparable to `now_ns()`.
- **`SymbolId = uint32_t`**, dense, usable as a direct array index.
- **`order_ref_t = uint64_t`** — the exchange's order reference, and the book
  key. Deliberately distinct from `order_id_t` (`uint64_t`), which would be
  *our own* outbound order id: they are different namespaces, and conflating
  them is how outbound acks get matched to the wrong resting order.
- **No separate `shares_t`.** ITCH shares are `uint32` on the wire; they widen
  into `qty_t` at decode. One quantity type is simpler than two.

## `stock_locate` is already a `SymbolId`
ITCH is a **single stream carrying all ~8,000 Nasdaq symbols**. Every message
carries a `stock_locate` (`uint16`), a dense id assigned by the `R`
StockDirectory messages at start of day. That is exactly what `SymbolId` is, so
the 8-char symbol string is never parsed on the hot path — it is read once from
the `R` messages, and everything downstream indexes by locate.

**How this works today:** `BookSet` holds a flat `by_locate_` array
(`1 << 16` entries) and resolves `stock_locate → OrderBook*` with a bounds check
and one load (`src/book_set.cpp`). The 8-char symbol is compared against the
watchlist only in `on_directory`, at start of day.

`SymbolTable` (intern / lookup / name, plus `tick_size` / `qty_increment` /
`scale`) is defined in `types.hpp` but **not currently used by anything**. It
predates the ITCH switch and is crypto-shaped: US equities are uniformly
\$0.0001 on the wire with a \$0.01 display tick, and shares are integers, so the
per-symbol metadata it carries has no consumer. It is either the generalization
point for a second venue or dead code — that decision is open.

## Done checklist
- [x] `nanos_t` / `price_t` / `qty_t` / `SymbolId` / `Side` / `order_ref_t`
- [x] Event representation decided: no event struct (feed-handler.md §1)
- [x] `BookDelta` removed
- [x] Symbol resolution on the hot path: `stock_locate` → `BookSet::get`
- [ ] Decide `SymbolTable`'s fate: wire it up for a second venue, or delete it
      along with `tick_size`/`qty_increment`/`scale`
