# Design: MoldUDP64 Transport, Gap Detection & Feed Simulator

**File(s):** none · **Phase:** 1 · **Status:** ⬜ not built

Design-time notes for the transport layer beneath the ITCH decoder. **None of
this exists in the code.** Replay today reads a BinaryFILE capture directly from
disk, single-threaded, with no sockets and no sequence numbers — see
[../feed-handler.md](../feed-handler.md) for what is built.

This document records the intended shape so it is not re-derived later.

---

## 1. Why a transport layer at all

The capture file is Nasdaq **BinaryFILE** — `[u16 BE length][payload]` back to
back, with no session header and **no sequence numbers**. MoldUDP64 is the live
multicast transport, and it is what carries the sequencing:

```
[10-byte session][uint64 sequence][uint16 message count]
  then N × [uint16 length][payload]
```

**Gap detection only exists at the Mold layer.** That is the entire argument for
building this: handling gaps is what separates a feed handler from a file
parser. Parsing the file directly — which is what happens today — can never
exercise that path.

So the work splits into two components:

1. **Feed simulator** (separate process, not hot path): reads BinaryFILE, packs
   messages into MoldUDP64 packets, sends to a UDP socket (multicast on loopback
   is fine). Real desks build exactly this for testing. Because the simulator is
   ours, gaps, reorders, and duplicates can be **injected on demand** — something
   no live venue permits.
2. **Feed handler** (hot path): `recvmmsg` → MoldUDP64 decode (sequence
   tracking, gap detection) → ITCH decode → book.

Keeping the simulator out-of-process is what makes the socket read real rather
than a function call.

---

## 2. Sequence-gap handling

The sequence number lives in the **MoldUDP64 packet header**, not in ITCH
messages. Each packet declares its first sequence number and a message count, so
next expected = `seq + count`.

A gap means packets were lost. The real feed offers a **retransmit request** to a
recovery service; the simulator can implement the same request/response.

The handler's job is to **detect** the gap and emit a signal (e.g.
`FeedEvent::GapDetected`) that the wiring layer routes to recovery. It does
**not** perform the recovery fetch — that is blocking I/O, and belongs on a cold
thread (ARCHITECTURE §3).

**Sequence numbers do not belong in a normalized event.** A sequence number is a
property of the *packet*, not of an individual book update. One packet carries
one sequence number but many messages, so putting it on the event would
duplicate it across every event from that packet — a sign it is on the wrong
object. Feed integrity is answered *inside* the handler before anything reaches
the book; downstream only cares whether the book is **valid**.

So `last_sequence_num` is per-feed state in the handler — which is why the
handler is a class rather than a free function. What flows downstream on a gap is
a **signal**, not the number.

**Recovery semantics.** ITCH carries no in-feed snapshot, so a diverged book is
never repaired in place. The correct response is to stop quoting the symbol,
re-snapshot via GLIMPSE (a separate TCP service returning the book plus the
sequence number it corresponds to), and replay forward from there.

---

## 3. What replay can and cannot prove

Worth stating precisely, because it bounds every latency claim the project makes.

**Faithful under replay:** the wire format exactly; L3 order-by-order book
semantics; zero-allocation hot path, cache behavior, branch layout; decode +
book-update cost per message. Once this transport exists, gap detection and
recovery join that list.

**Not obtainable under replay, at any level of effort:**

- **Kernel bypass.** Production uses Solarflare/Onload, DPDK, or an FPGA that
  never enters the kernel. `recvmmsg` on loopback pays syscall and softirq costs
  a real box does not — often the largest slice of the real budget.
- **Arrival dynamics.** Replay delivers as fast as the reader consumes. Real
  feeds arrive in **microbursts**, which is exactly when queues back up and
  p99.9 blows out. Constant-rate replay hides the worst case.
- **Colocation / physical layer.** NIC hardware timestamping, PTP sync,
  cross-connect length. `recv_ts` is not an exchange→engine latency.
- **Tick-to-trade.** Market data in is one half; the outbound path is the other.
- **A/B feed arbitration.** Real ITCH is two redundant multicast feeds raced
  against each other.

**Two upgrades that would close part of the gap**, both simulator-side: A/B
arbitration (two simulator instances, independent injected drops, arbitrate by
sequence), and **paced replay** at recorded inter-arrival times so microbursts
are reproduced and p99.9 reflects them.

---

## 4. `recv_ts` must be stamped in the reader, not the parser

```cpp
recvmmsg(...);
nanos_t recv_ts = platform::now_ns();   // reader thread, BEFORE decoding
handler.decode<false>(packet, recv_ts, books);
```

Stamping inside the parser would exclude decode time from the measurement,
understating latency. The `decode` signature already takes `recv_ts` as a
parameter for exactly this reason — the caller owns it, and the caller must be
the reader loop.

Under replay there is no network and no exchange floor, so `recv_ts` measures
the simulator rather than an exchange. Never report `recv_ts − exchange_ts` as a
latency: the ITCH timestamp is ns since midnight ET on Nasdaq's clock, from a
day in 2020, and the two clocks are unrelated. See `../latency-harness.md`.

---

## 5. Done checklist

- [ ] MoldUDP64 framing + sequence tracking
- [ ] Gap detection + `GapDetected` signal out to the wiring layer
- [ ] Feed simulator: BinaryFILE → MoldUDP64 → UDP, with **injectable gaps /
      reorders / duplicates**
- [ ] Socket reader stamping `recv_ts` before decode
- [ ] (Later) A/B arbitration across two simulator instances
- [ ] (Later) Paced replay at recorded inter-arrival times
