# Module: Feed Handler (Nasdaq TotalView-ITCH 5.0)

**File(s):** `include/hft/feed_handler.hpp`, `src/feed_handler.cpp`
**Phase:** 1 · **Status:** 🟩 implemented — BinaryFILE length-prefixed framing +
ITCH decode for A/F/D/C/E/X/U/P, stock directory, optional per-message timing
tap; benchmarked, see `docs/benchmarks.md`.

> **Scope.** This document covers the decoder as built: framing, field
> extraction, and dispatch into the book. The transport beneath it — MoldUDP64,
> sequence-gap detection, and the feed simulator — is **not built**; its design
> lives in [design/feed-transport.md](design/feed-transport.md).

## Responsibility
The only component that understands the wire format. Bytes → book mutations,
with nothing downstream ever seeing a wire byte. Feed integrity (gap detection,
recovery signalling) belongs to this component too, but requires the transport
layer that is not yet built.

---

## 1. Event representation: there is no event struct — the decoder calls the book directly

**Decision: no intermediate event type.** The book exposes one method per
message effect, and the decoder's `switch` calls them directly. The event
*is* the function call; its arguments are exactly the fields that message
carries — nothing uninitialized, nothing to switch on twice.

```cpp
// decoder, having identified the type byte:
case 'A': book.add_order(ref, side, price, shares);              break;
case 'D': book.delete_order(ref);                                break;
case 'E': book.execute_order(ref, shares);                       break;
case 'X': book.cancel_order(ref, shares);                        break;
case 'U': book.replace_order(old_ref, new_ref, new_price, new_shares); break;
// ...
```

Same thread, same binary: the "event" never gets stored or crosses a queue
— it lives on the stack for the microsecond between decode and apply, so an
intermediate struct + `std::visit` dispatch would only add a copy across a
boundary that isn't there. The feed handler is thereby coupled to the book's
interface, which is fine when both are versioned together.

(The full reasoning — why the old `BookDelta` price-level type couldn't
represent L3 ITCH, and why a `std::variant` normalized event isn't worth it
here — is in `design-decisions.md §8`.)

---

## 2. Framing: the capture is BinaryFILE

The downloaded file is Nasdaq **BinaryFILE** — the format the decoder actually
reads:

```
[uint16 big-endian length][payload] [uint16 BE length][payload] ...
```

Back to back, with **no session header and no sequence numbers**. Verified: the
file opens `00 0c 53` → length `12`, type `'S'` (SystemEvent), matching the
spec's 12 bytes.

`decode` walks this framing directly off an in-memory buffer. Because there are
no sequence numbers at this layer, **gap detection is impossible here** — it
exists only in MoldUDP64, which is why the transport layer is a separate
component rather than a flag on this one. See
[design/feed-transport.md](design/feed-transport.md).

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

## 4. Zero-allocation

With no event struct (§1) there is no output buffer to return — the decoder
walks the buffer and calls book methods in place. Nothing to allocate on the
message path, by construction. The signature as built:

```cpp
// applies every framed message in the buffer; returns messages applied
template<bool Timing>
std::size_t decode(std::span<const std::byte> buffer, nanos_t recv_ts,
                   BookSet& books, LatencySink* sink = nullptr) noexcept;
```

Three things about that signature are deliberate. It takes a **`BookSet`**, not
a single `OrderBook`: ITCH is one stream carrying every symbol, so dispatch by
`stock_locate` happens inside the decoder and messages for symbols outside the
watchlist early-out. It is **templated on `Timing`** so the instrumentation
compiles out of production builds entirely (design-decisions.md §11). And the
`LatencySink*` is null in the untimed instantiation, where it costs nothing.

The zero-alloc discipline still has to hold **inside the book**: `add_order`
inserts into the order-ref map and a price level, `delete_order` removes
them. Back these with pre-sized/pooled storage, not per-call `new`. The
`replace_order`/`add_order` path is where an accidental `std::map` node
allocation would hide.

Verify it: override `operator new` in the test binary and count. Replay the
fixture through `decode`; the counter must not move after warm-up. That
turns "no hot-path allocation" from an intention into a test.

### `recv_ts` is the caller's responsibility

`decode` takes `recv_ts` as a parameter rather than stamping it internally.
Stamping inside the parser would exclude decode time from any latency measured
against it. Under the current file replay there is no reader thread, so the
benchmarks pass a literal; once a socket reader exists it must stamp before
calling in (design/feed-transport.md §4).

`recv_ts` and the ITCH message timestamp are **not** comparable: the latter is
ns since midnight ET on Nasdaq's clock from a day in 2020. Never subtract one
from the other — see `latency-harness.md`.

---

## 5. The data

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

## 6. Done checklist

**Decided (§1)**
- [x] Event representation: no event struct; decoder `switch` calls book
      methods directly. `U` → single `replace_order` (§3.1). `BookDelta`
      dropped from the codebase; rationale lives in `design-decisions.md §8`.

**Decode was built first, deliberately.** It is pure logic — testable against a
fixture with an independent decoder as oracle and no socket to debug. Transport
comes after, and only once decode is proven.

**Decode**
- [x] BinaryFILE reader: `[u16 BE len][payload]`
- [x] ITCH decoder: explicit offsets + `bswap`, **no packed structs**;
      `switch` on type byte calling book methods (§1)
- [x] L3 book from `A`/`F`/`E`/`C`/`X`/`D`/`U` — end-to-end fixture replay in
      `tests/test_feed_handler.cpp`, checked against an independent Python pass
      over the same bytes (293 482 framed messages; SPY book sane and uncrossed)
- [ ] Type-histogram test vs. spec lengths (reference: 9,979,810 msgs, 0
      mismatches over the first 295 MB) — verified by hand during bring-up but
      not pinned by a test
- [ ] Heap-allocation counter test: replay fixture, `operator new` count
      flat after warm-up (§4)

**Transport** — tracked in
[design/feed-transport.md](design/feed-transport.md) §5.
