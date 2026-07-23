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

// SPY book parameters. stock_locate is resolved at runtime from the 'R'
// (stock directory) messages, so the SymbolId here is only a label; the
// decoder keys off target_locate_, not this value. base_price/window match
// the production SPY config: 1 tick = 1 cent, base ~ $303.92, +/- $20.48.
constexpr SymbolId     kSpySym    = 7457;
constexpr price_t      kSpyBase   = 30392;  // cents
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

    OrderBook book(kSpySym, kSpyBase, kWindow, kPool);
    Handler handler;

    // recv_ts is unused by the current decoder (latency stamping is a later
    // concern); pass 0.
    const std::size_t frames = handler.decode(bytes, /*recv_ts=*/0, book);

    // Every message must frame cleanly: decode returns the count and must not
    // break early on a bad length.
    EXPECT_EQ(frames, kExpectedFrames);

    // Book-affecting SPY messages applied (post-locate-filter). Oracle from the
    // independent scan: A 285 + D 261 + E 17 + X 24 + U 15 = 602 (F and C are 0
    // for SPY; H/L/Y are not book-affecting and are not counted).
    EXPECT_EQ(handler.messages(), 602u);

    // After replay the SPY book should be non-empty and internally consistent:
    // a valid best bid and ask, with bid strictly below ask (no crossed book).
    const price_t bid = book.best_bid();
    const price_t ask = book.best_ask();
    EXPECT_NE(bid, kInvalidPrice) << "no resting bid after replay";
    EXPECT_NE(ask, kInvalidPrice) << "no resting ask after replay";
    if (bid != kInvalidPrice && ask != kInvalidPrice) {
        EXPECT_LT(bid, ask) << "book is crossed: bid " << bid << " >= ask " << ask;
    }
}

}  // namespace
}  // namespace hft
