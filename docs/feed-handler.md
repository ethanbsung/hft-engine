# Module: Feed Handler (Nasdaq TotalView-ITCH 5.0)

**File(s):** `include/hft/feed_handler.hpp`, `src/feed_handler.cpp`
**Phase:** 1 · **Status:** ⬜ ITCH decoder not started

## Responsibility
The only component that understands the wire format. Bytes → normalized
book events, and **feed integrity**: detect sequence gaps, signal when the
book must be discarded and recovered. Everything downstream speaks the
normalized event, never wire bytes.

---

## 1. Event representation: there is no event struct — the decoder calls the book directly

`BookDelta` (in `types.hpp`) cannot represent ITCH: its `new_qty` is a
**price-level** concept, but ITCH is **order-by-order (L3)** — `AddOrder`
carries an order reference, and `OrderExecuted` says "order `ref` traded N
shares" without ever naming a price. So `BookDelta` goes away as the feed's
output type.

The question was *what replaces it* — a tagged union, a `std::variant`, a
POD union. The answer, given two decisions we made:

- **One thread, no boundary** between feed handler and book. The "event"
  never gets stored, never enters a ring buffer — it lives on the stack for
  the microsecond between decode and apply. So it needs no fixed size, no
  trivial-copyability, no compact tag. Those were the only real arguments
  for a POD union over a variant, and they're gone.
- **No venue-agnostic requirement.** One venue, so ITCH's shape may leak.
  The "normalized event" abstraction can collapse entirely.

**Decision: no intermediate event type at all.** The book exposes one
method per message effect, and the decoder's `switch` calls them directly.
The event *is* the function call; its arguments are exactly the fields that
message carries — nothing uninitialized, nothing to switch on twice.

```cpp
// decoder, having identified the type byte:
case 'A': book.add_order(ref, symbol, side, price, shares);      break;
case 'D': book.delete_order(ref);                                break;
case 'E': book.execute_order(ref, shares);                       break;
case 'X': book.cancel_order(ref, shares);                        break;
case 'U': book.replace_order(old_ref, new_ref, new_price, new_shares); break;
// ...
```

The variant path would cost a struct construction + a `std::visit` dispatch
to cross a boundary that isn't there. Calling directly deletes both. This is
what in-process ITCH handlers actually do.

**Consequence:** the feed handler is coupled to the book's interface. That's
fine — same thread, same binary, versioned together. The coupling a
normalized event would have *added* (to avoid this coupling) was the
speculative part.

`types.md` and `order-book.md` were written against the price-level model
and the variant idea; both need updating to this decision.

---

## 2. Transport: the file is BinaryFILE, not MoldUDP64

The downloaded file is Nasdaq **BinaryFILE**:

```
[uint16 big-endian length][payload] [uint16 BE length][payload] ...
```

Back to back. **No session header, no sequence numbers.** Verified: file
opens `00 0c 53` → length `12`, type `'S'` (SystemEvent) = the spec's 12
bytes.

**MoldUDP64 is the live multicast transport**, and is *not* in the file:

```
[10-byte session][uint64 sequence][uint16 message count]
  then N × [uint16 length][payload]
```

So this is **two components**, and splitting them is the right shape:

1. **Feed simulator** (separate tool, not hot path): reads BinaryFILE,
   packs messages into MoldUDP64 packets, sends to a UDP socket (multicast
   on loopback is fine). Real desks build exactly this for testing.
2. **Feed handler** (hot path): `recvmmsg` → MoldUDP64 decode (sequence
   tracking, gap detection) → ITCH decode → book.

Why not just parse the file directly: **gap detection only exists at the
Mold layer**, and gap handling is what makes it a feed handler rather than
a file parser. You own the simulator, so you can inject gaps, reorders,
and duplicates **on demand** — something you can never do against a live
venue.

---

## 3. ITCH 5.0 decoding — the traps

**Prices are integers with 4 implied decimals.** `198400` = `$19.8400`.
No floats on the wire, no string→number conversion at all. Maps straight
onto `price_t = int64_t`.

**Timestamps are 6-byte big-endian ns since midnight ET.** There is no
`uint48`. Read 8 bytes and mask, or assemble from parts — watch for
reading past the end on the last message in a buffer.

**Everything is big-endian.** x86 is little-endian, so every multi-byte
field needs a byte swap (`__builtin_bswap16/32/64`, or `std::byteswap` in
C++23). **The single most common ITCH parser bug.**

**Do NOT `reinterpret_cast` a packed struct over the buffer.** It's the
obvious move for fixed-layout binary, and it's wrong here: the 6-byte
timestamp guarantees everything after it is **misaligned**.
`__attribute__((packed))` "works" on x86 but is UB by the standard and
generates worse code than expected. Read fields at **explicit offsets**
with `memcpy` + `bswap` — the optimizer collapses it to a single `mov` +
`bswap`. This is what fast production parsers do.

### Message set (`ref` = order reference = the book key)

| Type | Name | Len | Book effect |
|---|---|---|---|
| `A` | AddOrder | 36 | insert order at (price, side, qty) |
| `F` | AddOrderMPID | 40 | same as `A` + market-participant id |
| `E` | OrderExecuted | 31 | partial fill — reduce qty at `ref` |
| `C` | OrderExecutedWithPrice | 36 | fill at a different price |
| `X` | OrderCancel | 23 | partial cancel — reduce qty at `ref` |
| `D` | OrderDelete | 19 | remove `ref` entirely |
| `U` | OrderReplace | 35 | delete old `ref`, add **new** `ref` (§3.1) |
| `P` | TradeNonCross | 44 | non-displayable execution, no book change |
| `Q` | CrossTrade | 40 | auction cross |
| `S` | SystemEvent | 12 | start/end of day/session |
| `R` | StockDirectory | 39 | symbol definitions (start of day) |
| `H` | TradingAction | 25 | halts |
| `Y` | RegSHO | 20 | short-sale restriction |
| `L` | MarketParticipantPos | 26 | MM position (start of day) |

L3 book = `order_ref → (price, side, qty)` **plus** price levels.

### `AddOrder` ('A') field layout — verified by decode

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | message type `'A'` |
| 1 | 2 | stock locate |
| 3 | 2 | tracking number |
| 5 | 6 | timestamp (ns since midnight) |
| 11 | 8 | **order reference number** |
| 19 | 1 | buy/sell (`'B'`/`'S'`) |
| 20 | 4 | shares |
| 24 | 8 | stock symbol (space-padded ASCII) |
| 32 | 4 | price (4 implied decimals) |

Offsets are into the **payload** (after the 2-byte length prefix). Decoded
sample: `ts=14400000768178`, `ref=8`, `side=B`, `1500 VOD @ 19.8400`.

### 3.1 `U` (OrderReplace) is ONE book method, not delete-then-add in the decoder

`U` carries `old_ref, new_ref, new_price, new_shares` — but **no symbol and
no side** (like `D`, `E`, `X`, it identifies the order by ref and assumes
you know the rest). To insert the new order you must look up `old_ref` to
recover symbol+side *before* deleting it.

That lookup is **book semantics**, not framing. The decoder's job is offsets
and endianness; the moment it has to read the order map it's reaching into
book internals. So `U` maps to a single `book.replace_order(...)` that does
lookup → delete old → insert new internally — one place for the
"replace loses time priority" rule. Do **not** have the decoder call
`lookup` + `delete_order` + `add_order`.

(With a variant-event design, emitting two events would be cleaner because
the book stays a dumb applier. We deleted the variant; once the book owns
the methods and the order map, replace belongs inside it.)

---

## 4. Sequence-gap handling (the PnL-critical part)

The sequence number lives in the **MoldUDP64 packet header**, not in ITCH
messages. Each packet declares its first sequence number and a message
count → next expected = `seq + count`.

A gap means packets were lost. The real feed offers a **retransmit
request** to a recovery service; the simulator can implement the same
request/response.

The handler's job is to **detect** the gap and emit a signal (e.g.
`FeedEvent::GapDetected`) that the wiring layer routes to recovery. It does
**not** do the recovery fetch — that's blocking I/O, cold thread
(ARCHITECTURE §3).

**Sequence numbers do NOT belong in the normalized event.** A sequence
number is a property of the *packet*, not of an individual book update.
One packet carries one sequence number but many messages — putting it on
the event duplicates it across every event from that packet, a sign it's
on the wrong object. It's a feed-integrity question answered *inside* the
handler before any event reaches the book. Book/strategy/risk only care
whether the book is **valid**.

So: `last_sequence_num` is per-feed state in the handler (this is why the
handler is a class, not a free function). What flows downstream on a gap is
a **signal**, not the number.

---

## 5. Zero-allocation

With no event struct (§1) there is no output buffer to return — the decoder
walks the packet and calls book methods in place. Nothing to allocate on the
message path, by construction. The shape is roughly:

```cpp
// applies every message in the packet to `book`; returns messages applied
size_t decode(std::span<const std::byte> packet, nanos_t recv_ts,
              OrderBook& book) noexcept;
```

The zero-alloc discipline still has to hold **inside the book**: `add_order`
inserts into the order-ref map and a price level, `delete_order` removes
them. Back these with pre-sized/pooled storage, not per-call `new`. The
`replace_order`/`add_order` path is where an accidental `std::map` node
allocation would hide.

Verify it: override `operator new` in the test binary and count. Replay the
fixture through `decode`; the counter must not move after warm-up. That
turns "no hot-path allocation" from an intention into a test.

### `recv_ts` MUST be stamped in the reader, not the parser

```cpp
recvmmsg(...);
nanos_t recv_ts = platform::now_ns();   // reader thread, BEFORE decoding
handler.decode(packet, recv_ts, out);
```

If stamped inside the parser, decode time is excluded from the
measurement — understating (cheating) your latency. The signature makes
`recv_ts` the caller's responsibility; ensure the caller is the reader
loop. (In tests a literal like `12345` is fine.)

**Under replay, `recv_ts` measures the simulator, not an exchange.** Never
report `recv_ts − exchange_ts` as a latency: the ITCH timestamp is ns since
midnight ET on Nasdaq's clock, from a day in 2020.

⚠️ **ARCHITECTURE §2 no longer applies.** It argues your code's latency is
invisible against a ~5,000µs crypto exchange floor. Under replay there is
no network and no exchange floor — decode + book-update **is** the entire
measurable number. §2 should be rescoped or removed.

---

## 6. The data

`https://emi.nasdaq.com/ITCH/Nasdaq ITCH/` — plain HTTPS, **no auth**.
`01302020.NASDAQ_ITCH50.gz` = **5.6 GB compressed** (one trading day).

- gzip is a **single stream**: mid-file HTTP range requests will **not**
  inflate standalone. Stream it: `curl … | gunzip -c | tool`.
- The first **~2 MB is all start-of-day directory** (`R`/`H`/`Y`/`L`).
  No order flow until deeper in — don't test the book against the head of
  the file and conclude it's broken.

Message mix in the first 295 MB (9,979,810 messages, **0** spec-length
mismatches, **0** unknown types): 4.0M `A`, 3.95M `D`, 1.1M `X`, 577K `U`,
43K `E`, 31K `F`, 11K `P`.

**Specs:** `NQTVITCHSpecification.pdf`, `moldudp64.pdf` (nasdaqtrader.com).

---

## 7. Scope — what replay does and doesn't prove

**Gives you faithfully:** the wire format exactly; L3 order-by-order book
semantics; gap detection and recovery; zero-allocation hot path, cache
behavior, branch layout; honest decode + book-update cost per message.

**Cannot give you:**
- **Kernel bypass.** Production uses Solarflare/Onload, DPDK, or FPGA that
  never enters the kernel. `recvmmsg` on loopback pays syscall + softirq
  costs a real box doesn't — often the largest slice of the real budget.
- **Arrival dynamics.** Replay delivers as fast as you read. Real feeds
  arrive in **microbursts**, exactly when queues back up and p99.9 blows
  out. Constant-rate replay hides your worst case.
- **Colocation / physical layer.** NIC hardware timestamping, PTP sync,
  cross-connect length. `recv_ts` is not an exchange→you latency.
- **Tick-to-trade.** Market data in is one half; the outbound path is the
  other.
- **A/B feed arbitration.** Real ITCH is two redundant multicast feeds you
  race against each other.

**Defensible claim:** *production-grade feed handler and L3 order book,
benchmarked in isolation; kernel bypass and colocation out of scope for a
laptop.* Stronger than overselling — it shows you know the boundary.

**Two cheap upgrades that close part of the gap** (both simulator-side):
A/B arbitration (two simulator instances, independent injected drops,
arbitrate by sequence); and **paced replay** at recorded inter-arrival
times so microbursts are reproduced and p99.9 is real.

---

## 8. Done checklist

**Decided (§1)**
- [x] Event representation: no event struct; decoder `switch` calls book
      methods directly. `U` → single `replace_order` (§3.1).
- [ ] Update `types.md` + `order-book.md` to match (drop `BookDelta` as the
      feed output; the variant idea is off the table)

**Sequence: BinaryFILE-first, then Mold, then simulator.** Decode is pure
logic testable against the 281 MB fixture with the histogram as oracle and
no socket to debug. Mold framing and the simulator come only after decode is
proven.

**ITCH (do first)**
- [ ] BinaryFILE reader: `[u16 BE len][payload]`, streamed
- [ ] ITCH decoder: explicit offsets + `bswap`, **no packed structs**;
      `switch` on type byte calling book methods (§1)
- [ ] Type-histogram test vs. spec lengths (reference: 9,979,810 msgs, 0
      mismatches over the first 295 MB)
- [ ] L3 book from `A`/`F`/`E`/`C`/`X`/`D`/`U`
- [ ] Heap-allocation counter test: replay fixture, `operator new` count
      flat after warm-up (§5)

**Transport (after decode is proven)**
- [ ] MoldUDP64 framing + gap detection
- [ ] Feed simulator: BinaryFILE → MoldUDP64 → UDP, **injectable gaps /
      reorders / duplicates**
- [ ] (Later) A/B arbitration; paced replay
