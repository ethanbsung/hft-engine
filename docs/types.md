# Module: Types

**File(s):** `include/hft/types.hpp`
**Phase:** 1 · **Status:** 🟩 primitives in use throughout the engine. Symbol
resolution goes through `BookSet`'s `stock_locate` array (see below).

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
  **The decoder divides by 100**, so `price_t` in this engine holds
  **cents** (`19840`), not the wire's ten-thousandths: US equities quote in
  \$0.01 increments, and cents make the book's fixed price window index
  directly. Sub-penny prices (odd lots, price improvement) truncate — a real
  limitation, acceptable only while the book is display-tick-oriented.
- **`qty_t = int64_t`** in the smallest tradeable unit (ITCH: shares).
- **`nanos_t = int64_t` monotonic ns**, read via `platform::now_ns()` — though
  the latency harness times with `platform::read_cycles()` and stores raw
  cycle deltas, so `nanos_t` is currently only a signature type
  (`decode`'s `recv_ts`, which is `[[maybe_unused]]`). ITCH's own timestamp
  is **6-byte big-endian ns since midnight ET** — a wall-clock-ish venue
  timestamp, *not* comparable to either clock, and **never parsed**: the
  `load_be_u48` helper that would read it has no callers.
- **`SymbolId = uint32_t`**, dense, usable as a direct array index — the
  general symbol-identity type. On the ITCH hot path the concrete id is
  `stock_locate`, carried as the `uint16_t` the wire provides
  (`BookSet::get`), so no widening happens per message.
- **`order_ref_t = uint64_t`** — the exchange's order reference, and the book
  key. Deliberately distinct from `order_id_t` (`uint64_t`), which would be
  *our own* outbound order id: they are different namespaces, and conflating
  them is how outbound acks get matched to the wrong resting order.
  `order_id_t` is **defined but unused** — there is no outbound order path
  yet, so the distinction is currently intent, not enforcement.
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

**No per-symbol metadata table.** US equities are uniformly \$0.0001 on the
wire with a \$0.01 display tick, and shares are integers — so there is nothing
per-symbol to look up, and no interning step: the wire hands over a dense id
directly. A venue with heterogeneous tick sizes or fractional quantities (most
crypto) would need that table; ITCH does not, and adding one now would be
carrying a lookup the hot path never performs.

## Done checklist
- [x] `nanos_t` / `price_t` / `qty_t` / `SymbolId` / `Side` / `order_ref_t`
- [x] Event representation decided: no event struct (feed-handler.md §1)
- [x] `BookDelta` removed
- [x] Symbol resolution on the hot path: `stock_locate` → `BookSet::get`
