# Module: Risk (pre-trade checks)

**File(s):** `include/hft/risk.hpp` (to create)
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
The **gate every order passes through before it leaves the system.**
Rejects orders that violate limits. Sits between Strategy and the
gateway/simulator on the hot thread (ARCHITECTURE §5). Fast, deterministic,
no allocation, no I/O.

## Checks to implement (the standard pre-trade set)
1. **Position limits** — per-symbol and aggregate. Reject an order that
   would push net position past the max.
2. **Price reasonability** ("fat-finger") — reject orders priced far from
   the current market (e.g. more than X ticks / Y% from mid). Guards
   against a bad quote from a broken book.
3. **Order size limits** — max qty per order.
4. **Rate limiting (token bucket)** — cap orders/second so you don't spam
   the exchange and trip its penalties. Refill tokens over time; reject
   when the bucket is empty. This is a genuinely real HFT concern
   (exchanges penalize excessive messaging).
5. *(optional)* **Max open orders** / max notional exposure.

## Design notes
- Risk owns or reads the **authoritative position** (updated by fills from
  the execution simulator). Position is shared state — the strategy
  *reads* it, risk *enforces* against it, fills *update* it. Decide the
  ownership and update path and document it (the "position drift" trap —
  keep one source of truth).
- **Fail closed:** if a check can't be evaluated (e.g. book invalid /
  gapped), reject rather than pass.
- Return a **reason** on rejection (for logging/telemetry), not just a bool.

## Interface (contract sketch)
```cpp
enum class RiskDecision { Accept, RejectPosition, RejectPrice,
                          RejectSize, RejectRate, RejectBookInvalid };

class RiskEngine {
public:
    RiskDecision check(const Order& o, const Position& pos,
                       const OrderBook& book) noexcept;
    void on_fill(const Fill&) noexcept;   // update position
};
```

## Latency notes
Hot-path, but it's a handful of comparisons + a token-bucket update —
cheap. Keep it branch-light; no logging on the hot path (push reject
reasons to the telemetry ring buffer).

## Done checklist
- [ ] Position limit (per-symbol + aggregate)
- [ ] Price reasonability / fat-finger
- [ ] Order size limit
- [ ] Token-bucket rate limiter
- [ ] Fail-closed on invalid/gapped book
- [ ] Reason codes on rejection
- [ ] Tests: each check accepts valid / rejects the specific violation,
      token bucket refills correctly over time
