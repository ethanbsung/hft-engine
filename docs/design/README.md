# docs/design — design-time notes for unbuilt components

Every document in this directory describes a component that is **not built**.
They were written before implementation and record *intent*: the responsibility,
the interface sketch, the data structures under consideration, the latency
concerns, and a "done" checklist. They are not descriptions of shipped code, and
nothing here has been measured.

| Doc | Component | Why it's here |
|---|---|---|
| [ring-buffer.md](ring-buffer.md) | SPSC lock-free queue | Needed only once there is more than one thread; replay is single-threaded today. |
| [threading.md](threading.md) | Thread layout & wiring | Same — the hot/cold split has nothing to split yet. |
| [strategy.md](strategy.md) | Market-making signal | Downstream of the book; the book came first. |
| [risk.md](risk.md) | Pre-trade risk checks | Downstream of strategy. |
| [execution-simulator.md](execution-simulator.md) | Fill model, PnL, position | Downstream of risk; the queue-position rationale for an L3 book lives here. |
| [order-gateway.md](order-gateway.md) | Live order entry | **Out of scope** — ITCH is market-data-only, there is no venue to send to. Kept to record the design seam. |

For what *is* built and measured, see [../benchmarks.md](../benchmarks.md),
[../order-book.md](../order-book.md), and [../feed-handler.md](../feed-handler.md).
The authoritative build status for every module is the index in
[../ARCHITECTURE.md](../ARCHITECTURE.md) §8.
