# Module: Threading & Wiring

**File(s):** `main.cpp` + a small `engine`/`runner` (to create)
**Phase:** 1 · **Status:** ⬜ not started

## Responsibility
The glue: create the threads, pin them, wire the ring buffers between them,
own the run loop, and handle startup/shutdown cleanly. This is where the
single-hot-thread decision (ARCHITECTURE §3) becomes real.

## Thread layout (Phase 1)
| Thread | Loop | Notes |
|---|---|---|
| **Socket reader** | read WS frame → stamp `recv_ts` → push to hot ring | blocking I/O isolated here |
| **Hot thread** | pop frame → parse → book → strategy → risk → sim → record latency | the critical path, **one thread**, busy-poll its input |
| **Order sender** | pop order → send (Phase 1.5: real; Phase 1: no-op/sim internal) | blocking I/O isolated here |
| **Logger/telemetry** | drain telemetry ring → aggregate → write | never touches hot path |

In **Phase 1**, the execution simulator runs *on the hot thread* (it's your
own cheap model, not I/O), so the "order sender" thread is mostly a
Phase-1.5 concern. Keep the seam anyway.

## Busy-poll vs. block
- **Hot thread busy-polls** its input ring (spin reading, no sleep) for
  deterministic latency — trade a burned core for low jitter. Only do this
  on the pinned Linux perf runs; on the Mac dev box a light sleep/yield is
  fine (you're not measuring there).
- **I/O threads block** on the socket (that's the point of isolating them).
- Provide a clean way to **stop** (atomic `running` flag checked in loops).

## Pinning
- `platform::pin_thread_to_core(id)` — real on Linux, no-op on macOS
  (already implemented in `platform.cpp`). Pin the hot thread to an
  **isolated** core (`isolcpus`) on the perf box; document which core.
- Keep the hot thread off the same core as the I/O threads.

## Shutdown ordering
Stop producers before consumers; drain rings; join threads; flush the
logger last so you don't lose the final telemetry.

## Design notes
- Rings are the *only* cross-thread communication. No shared mutable state
  across threads except through a ring or a carefully-documented atomic
  (e.g. `running`).
- Position/PnL is read by strategy/risk on the **same** hot thread that
  updates it (via sim fills), so in Phase 1 it needs **no** cross-thread
  synchronization — call that out, it's a nice simplification the single-
  thread design buys you. (It changes when the live gateway makes fills
  async — plan for it, don't pay for it yet.)

## Done checklist
- [ ] Thread creation + pinning + clean shutdown
- [ ] Rings wired: reader→hot, hot→telemetry (hot→sender in P1.5)
- [ ] Hot thread busy-poll loop (Linux) / yield loop (Mac dev)
- [ ] `running` flag + graceful stop + join order + final flush
- [ ] Runs the full Phase-1 loop end to end against captured data
