#include <gtest/gtest.h>

#include "hft/feed_handler.hpp"
#include "hft/orderbook.hpp"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

// End-to-end feed-handler test. Reads a real Nasdaq TotalView-ITCH 5.0
// order-flow fixture, replays it through Handler::decode into an OrderBook,
// and checks the decoder framed every message and reconstructed a sane book.
//
// The fixture (tests/fixtures/itch_orderflow.bin) is gitignored: it is large
// and re-downloadable, so it is not committed. When it is absent the test
// SKIPs rather than fails, so CI without the file stays green.
//
// Oracle values were established by an independent Python pass over the same
// bytes:
//   - total framed messages: 293482
//   - SPY stock_locate:      7457
// The book is constructed for SPY only; decode()'s locate filter drops every
// other symbol, so only SPY's ~600 book-affecting events reach the book.

namespace hft {
namespace {

constexpr const char* kFixturePath = "tests/fixtures/itch_orderflow.bin";

// Oracle: number of length-framed messages in the whole fixture.
constexpr std::size_t kExpectedFrames = 293482;

// SPY resolves to stock_locate 7457 in this fixture (locate is not stable
// across days; it is resolved at runtime from the 'R' messages). window/pool
// are the per-book config; each book auto-centers its price ring on its first
// order, so no base_price is supplied here. 1 tick = 1 cent, +/- $20.48.
constexpr SymbolId     kSpySym    = 7457;
constexpr std::size_t  kWindow    = 4096;
constexpr std::size_t  kPool      = 1 << 16;

std::vector<std::byte> read_fixture(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize n = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

TEST(FeedHandler, ReplaysItchFixture) {
    const std::vector<std::byte> bytes = read_fixture(kFixturePath);
    if (bytes.empty()) {
        GTEST_SKIP() << "fixture missing: " << kFixturePath;
    }

    // BookSet builds a per-symbol book for every watchlist symbol as its 'R'
    // directory message is decoded; SPY is on the watchlist. window/pool apply
    // to each book it creates.
    BookSet books(kWindow, kPool);
    Handler handler;

    // recv_ts is unused by the current decoder (latency stamping is a later
    // concern); pass 0.
    const std::size_t frames = handler.decode<false>(bytes, /*recv_ts=*/0, books);

    // Every message must frame cleanly: decode returns the count and must not
    // break early on a bad length.
    EXPECT_EQ(frames, kExpectedFrames);

    // Book-affecting messages applied across ALL watchlist symbols present in
    // the fixture (SPY, QQQ, MSFT, AAPL, TSLA, GOOGL, AMZN). Oracle from the
    // independent scan: SPY 602 + QQQ 905 + TSLA 475 + AAPL 131 + MSFT 71 +
    // AMZN 57 + GOOGL 31 = 2272.
    EXPECT_EQ(handler.messages(), 2272u);

    // SPY resolves to stock_locate 7457 in this fixture; its book is created
    // from the 'R' message. Fetch it from the set and check consistency.
    OrderBook* spy = books.get(kSpySym);
    ASSERT_NE(spy, nullptr) << "SPY book was not created from directory";

    // After replay the SPY book should be non-empty and internally consistent:
    // a valid best bid and ask, with bid strictly below ask (no crossed book).
    const price_t bid = spy->best_bid();
    const price_t ask = spy->best_ask();
    EXPECT_NE(bid, kInvalidPrice) << "no resting bid after replay";
    EXPECT_NE(ask, kInvalidPrice) << "no resting ask after replay";
    if (bid != kInvalidPrice && ask != kInvalidPrice) {
        EXPECT_LT(bid, ask) << "book is crossed: bid " << bid << " >= ask " << ask;
    }

    // Every watchlist symbol present in the fixture must dispatch to its own
    // book and stay internally consistent. This guards against a dispatch bug
    // (e.g. messages routed to the wrong locate) that a SPY-only check misses.
    // Thin symbols (few messages) may end one-sided or empty, so the invariant
    // is: the book exists, and if both sides are present it is not crossed.
    // Locates resolved from the fixture's 'R' messages by the same scan that
    // produced the 2272 oracle above.
    struct WatchLocate { const char* name; uint16_t locate; };
    constexpr WatchLocate kResolved[] = {
        {"AAPL", 13}, {"AMZN", 398}, {"GOOGL", 3461}, {"MSFT", 5294},
        {"QQQ", 6562}, {"SPY", 7457}, {"TSLA", 8000},
    };
    for (const auto& w : kResolved) {
        OrderBook* b = books.get(w.locate);
        ASSERT_NE(b, nullptr) << w.name << " book was not created from directory";
        const price_t wb = b->best_bid();
        const price_t wa = b->best_ask();
        if (wb != kInvalidPrice && wa != kInvalidPrice) {
            EXPECT_LT(wb, wa) << w.name << " book is crossed: bid " << wb
                              << " >= ask " << wa;
        }
    }
}

}  // namespace
}  // namespace hft
