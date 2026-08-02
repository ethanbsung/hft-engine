# Module: Execution Simulator

**File(s):** `include/hft/execution_simulator.hpp` (to create)
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
Stand in for the exchange. Takes approved orders, decides **if/when/how
much they fill** against the replayed book, and produces **fills →
position + PnL**. This closes the loop end-to-end.

**This is now the only execution path.** Nasdaq ITCH replay is
market-data-only: there is no venue to send orders to. Keep the interface
abstract anyway (a base class or concept) so a live gateway *could* drop
in — it costs nothing and documents the boundary.

## Fill model — L3 makes queue position exact
The quality of the simulator is the quality of your backtest. Order-by-order
data changes what's possible here, and this is the **single biggest payoff
of using ITCH**:

- **Queue position is no longer an approximation.** With L2 you could only
  guess how much size rested ahead of you at your price. ITCH tells you
  *every individual order* at a level and the order in which they arrived.
  So you know **exactly** how many shares are ahead of your simulated order,
  and you can consume that queue correctly as `E`/`X`/`D` messages arrive.
  This is the realism upgrade that L2 feeds fundamentally cannot support —
  and a very good thing to be able to explain.
- **Model it properly:** when you place a simulated resting order at price
  P, record the aggregate qty already resting at P (from the per-level
  FIFO — see order-book.md). You fill only after that quantity is consumed
  by executions and cancels ahead of you.
- **Cancels ahead of you help you.** An `X`/`D` on an order in front
  advances your queue position without any trade happening. Getting this
  right is what separates a real queue model from a toy one.
- **`U` (replace) loses time priority.** An order that replaces goes to the
  *back* of the queue at its new price. If you model your own orders'
  replaces, they must lose priority too — otherwise the sim is optimistic.
- **`E` / `C` are the fill triggers.** These are executions against resting
  orders. If the executed order is ahead of you, decrement your queue; when
  the queue ahead reaches zero, you fill.
- **No fills better than the market.** Never let the sim be optimistic.

**Fees:** ITCH carries no fee schedule, and US equity maker/taker rebates
are venue- and tier-specific. Either model a flat per-share fee/rebate as a
parameter, or **report gross PnL and say so**. Do not invent a number and
present it as real.

## PnL & position
- **Position:** running signed inventory per symbol (updated on fill).
- **Realized PnL:** from round-trips.
- **Unrealized/mark-to-market PnL:** inventory × (mark − avg entry), marked
  at mid. Report both.
- This position/PnL is the **same state Risk enforces against and Strategy
  skews on** — one source of truth (see risk.md).

## Interface (contract sketch)
```cpp
class ExecutionSimulator {  // same interface a live gateway would implement
public:
    void submit(const Order&) noexcept;
    void cancel(order_id_t) noexcept;
    // driven by market data to check for fills:
    void on_event(const Event&) noexcept;   // Add/Execute/Cancel/Delete/Replace
    // emits Fill events → position/PnL
};
```

Note `order_id_t` (our own outbound id) is distinct from `order_ref_t`
(the exchange's reference on real orders in the book) — see types.md.

## Design notes
- No separate trade stream is needed: **`E`/`C` execution messages are the
  trade stream**, and they identify the resting order that traded. (`P`
  TradeNonCross reports non-displayable executions and does **not** touch
  the book — don't fill against it.)
- The simulator needs the **per-level FIFO of order refs** from the order
  book to compute queue position. Decide whether it reads the book's
  structure or the book exposes a `qty_ahead_of(price, arrival)` query.

## Latency notes
Not latency-critical (it's your own model), but keep it on the hot thread
for Phase 1 simplicity; the fill check runs per market event.

## Done checklist
- [ ] Submit / cancel simulated resting orders
- [ ] Queue position from the per-level FIFO (qty resting ahead at entry)
- [ ] Consume queue on `E`/`C` (executions) **and** `X`/`D` (cancels ahead)
- [ ] `U` replace → back of queue (no free priority)
- [ ] Fill when queue ahead reaches zero; partial fills
- [ ] Ignore `P` (non-displayable) for fills
- [ ] Position + realized + unrealized PnL
- [ ] Fees: parameterized, or report gross and label it
- [ ] Shared position is the single source of truth (Risk/Strategy read it)
- [ ] Tests: queue advance on cancel-ahead, partial fill, no over-optimistic
      fill, replace-loses-priority, PnL math
