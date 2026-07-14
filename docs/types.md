# Module: Types & SymbolTable

**File(s):** `include/hft/types.hpp`
**Phase:** 1 · **Status:** ✅ started (primitives + SymbolTable done)

## Responsibility
Define the vocabulary the entire system speaks: time, price, quantity,
side, the exchange order reference, and the `SymbolTable` that maps symbol
names ↔ ids and holds per-symbol market metadata.

## `BookDelta` is gone — there is no normalized event type
`BookDelta`'s `new_qty` ("absolute resting qty at a price level") is a
**price-level (L2)** concept. Nasdaq ITCH is **order-by-order (L3)**:
`AddOrder` carries an order reference; `OrderExecuted` says "order `ref`
traded N shares" and never mentions a price. `BookDelta` cannot express it.

**Resolved (feed-handler.md §1): there is no replacement event struct at
all.** With one thread (no feed→book boundary) and one venue, the decoder's
`switch` calls book methods (`add_order`, `delete_order`, …) directly. So
`types.hpp` defines the *primitives* those method signatures are built from,
not a tagged-union event. The variant/POD-union idea is off the table.

Delete `BookDelta` from `types.hpp` when you cut over.

Types the ITCH book method signatures need:
- **`order_ref_t = uint64_t`** — the exchange's order reference. The book
  key. (Distinct from `order_id_t`, which is *our own* outbound order id.)
- **`shares_t`** or reuse `qty_t` — ITCH shares are `uint32`.

## Design decisions (already made)
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
- **`SymbolTable`** carries per-symbol metadata; the generalization point
  across instruments.

## `stock_locate` is already a `SymbolId`
ITCH is a **single stream carrying all ~8,000 Nasdaq symbols**. Every
message carries a `stock_locate` (`uint16`), a dense id assigned by the
`R` StockDirectory messages at start of day. That is exactly what
`SymbolId` is — so on the hot path you should **never parse the 8-char
symbol string**; intern once from the `R` messages, index by locate
thereafter.

This makes `SymbolTable::lookup(string_view)` a **start-of-day / cold-path**
call, not a hot-path one. (The heterogeneous-lookup work is still correct,
just no longer on the critical path.)

`tick_size` / `qty_increment` / `scale` were crypto-shaped: US equities are
uniformly $0.0001 on the wire with a $0.01 display tick, and shares are
integers. Revisit whether these fields still earn their place.

## Known future work (recorded, not urgent)
- `intern` currently stores the name twice (in `idToName` and as the map
  key). Fine for now; revisit if it matters.
- Heterogeneous lookup is wired (`StringHash` + `equal_to<>`), so
  `lookup(string_view)` avoids allocating — good, keep it (cold path now).

## Done checklist
- [x] `nanos_t` / `price_t` / `qty_t` / `SymbolId` / `Side` defined
- [x] `SymbolTable` intern / lookup / name
- [x] Event representation decided: no event struct (feed-handler.md §1)
- [ ] Delete `BookDelta` from `types.hpp`
- [ ] Add `order_ref_t`; keep it distinct from our own `order_id_t`
- [ ] Populate `SymbolTable` from `R` StockDirectory; key by `stock_locate`
- [ ] Decide whether `tick_size`/`qty_increment`/`scale` survive
- [ ] Unit test for `SymbolTable` round-trip (intern → lookup → name)
