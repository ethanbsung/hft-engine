# dead/ — retired code

Not built, not tested, not included by anything. Kept for reference only.

These files implemented the **Coinbase WebSocket JSON L2 feed**, which was
abandoned in favor of **Nasdaq TotalView-ITCH 5.0** replayed over
MoldUDP64/UDP (see `docs/feed-handler.md`).

| File | Was | Why retired |
|---|---|---|
| `src/feed_handler.cpp` | `l2_data` JSON → `BookDelta` via nlohmann | ITCH is binary; no JSON parse at all |
| `include/hft/feed_handler.hpp` | its header | replaced by the ITCH decoder |
| `tests/test_feed_handler.cpp` | GoogleTest for the above | tests a deleted API |
| `tools/capture_feed.cpp` | Boost.Beast WebSocket+TLS capture tool | no live feed to capture |
| `tests/fixtures/btc_usd_l2.jsonl` | 7.3 MB captured Coinbase L2 (untracked) | wrong venue, wrong format |

## What went away with them

- **nlohmann/json** — the only consumer was the JSON parser. ITCH prices
  arrive as `uint32` with 4 implied decimals, so there is no string→number
  conversion anywhere on the hot path.
- **Boost + OpenSSL** — only `capture_feed` needed them (WebSocket + TLS).
- **`HFT_BUILD_TOOLS`** — `tools/` is now empty.

## Still worth reading

`src/feed_handler.cpp` is a compact example of the allocation problems the
new design exists to avoid: a DOM parse per message, a returned `vector`, a
`std::string` copy just to hash a symbol, and `std::stod` (locale-aware,
allocating) feeding a float→int rounding on the path to an integer tick.
`docs/feed-handler.md` §5 is the answer to all four.

`tools/capture_feed.cpp` is a reasonable reference if you ever need a
Boost.Beast sync WebSocket+TLS client again.

## Deleting this folder

Nothing references it. `rm -rf dead/` is safe whenever you stop wanting the
reference. History is preserved either way — the files were moved with
`git mv`.
