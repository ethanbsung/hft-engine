# Module: Order Gateway (live)

**File(s):** none · **Phase:** out of scope · **Status:** ❌ not applicable

## Status: there is no live venue

Nasdaq TotalView-ITCH is a **market-data feed**. It has no order entry, and
this project has no live venue. **All fills come from the execution
simulator** (see execution-simulator.md).

This file is kept only to record the **design seam** and the live-trading
considerations, so neither is rediscovered later.

## The seam (why it still matters)

`ExecutionSimulator` implements the interface a live gateway *would*:

```cpp
class OrderGateway /* same base/concept as ExecutionSimulator */ {
public:
    void submit(const Order&) noexcept;
    void cancel(order_id_t) noexcept;
    // async: emits Ack / Fill events back to position/PnL
};
```

Keeping that boundary abstract (base class or concept) means strategy and
risk never learn whether fills are simulated or real. That is the payoff of
the instrument-agnostic design (ARCHITECTURE §4) and costs nothing to
maintain.

Note `order_id_t` (our own outbound id) is distinct from `order_ref_t`
(the exchange's reference on orders in the ITCH book) — see types.md.

## If a live venue is ever added (recorded, not planned)

- **Auth / secrets** in env or key file, never in code.
- **Sending is blocking I/O** → its own thread, fed from the hot thread via
  an SPSC ring (ARCHITECTURE §3). The hot thread never blocks on a socket.
- **Acks/fills are asynchronous** → normalize to `Fill`/`Ack` events and
  update the *same* position state Risk enforces against. Reconcile against
  what you *think* is live — the position-drift trap.
- **Idempotency / client order IDs:** tag every order so acks/fills match
  and retries don't double-send.
- **Cancel-on-disconnect / kill switch:** on any doubt about connection or
  book validity, cancel everything. Non-negotiable for live trading.
- **Rate limits** are a real pre-trade risk check, not an afterthought
  (risk.md).

> Realism note: pairing a Nasdaq equities feed with a crypto order gateway
> would be realistic for neither venue. If live trading ever matters, the
> feed and the gateway must be the **same venue** — which means an exchange
> relationship, not a signup.
