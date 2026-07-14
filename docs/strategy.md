# Module: Strategy

**File(s):** `include/hft/strategy.hpp` (to create)
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
Consume the order book view, produce **desired quotes/orders**. Phase 1
target: a **basic market-making** strategy — quote a bid below and an ask
above the mid, sized within risk limits, and manage inventory. Runs on the
hot thread as a plain function call after each book update.

## Scope for Phase 1 (keep it honest and small)
A defensible minimal market maker:
- Compute a **fair/mid price** from top of book.
- Quote **bid = mid − half_spread**, **ask = mid + half_spread** (in ticks).
- **Skew** quotes based on current inventory (lean to offload a long/short
  position — pull the quote that would grow the position, improve the one
  that would reduce it).
- Respect a **max position** and per-quote size from Risk.
- **Requote** on meaningful book moves; avoid requoting on noise
  (hysteresis) so you're not spamming cancel/replace.

This is enough to be a *real* strategy with real tradeoffs (spread vs.
fill rate, inventory risk) without turning into a research project.

## Interface (contract sketch)
```cpp
struct Quote { price_t bid_px; qty_t bid_qty; price_t ask_px; qty_t ask_qty; };

class Strategy {
public:
    // called on each book update; returns desired quotes (or "no change")
    Quote on_book_update(const OrderBook& book, const Position& pos) noexcept;
};
```
Keep it **pure-ish**: inputs are book + position, output is desired state.
Turning "desired quotes" into concrete new/cancel orders (diffing against
live orders) can live here or in a thin order-management layer — decide
and document.

## Design notes
- **Desired-state vs. order-diff:** the strategy is cleaner if it emits a
  *desired* quote and something else diffs it against resting orders to
  produce cancels/amends. Simplifies the strategy logic a lot.
- **No allocation, no branches you don't need** — it's on the hot path.
  But correctness and clarity first; this isn't the latency bottleneck.
- **Parameters** (spread, size, skew, max position) should be config, not
  magic numbers — makes it tunable and testable.

## Latency notes
Hot-path, but arithmetic-light. Fine to build straightforwardly. The
interesting latency work is upstream (book) and in the plumbing, not here.

## Done checklist
- [ ] Mid / fair price from top of book
- [ ] Symmetric quotes around mid (spread in ticks)
- [ ] Inventory skew
- [ ] Max-position / size respected (from Risk/Position)
- [ ] Requote hysteresis (don't spam on noise)
- [ ] Tests: quote placement, skew direction under long/short, no-quote
      when book invalid/gapped, position-limit clamp
