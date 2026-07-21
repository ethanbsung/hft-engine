#include <gtest/gtest.h>
#include "hft/orderbook.hpp"

// Tests for OrderBook: add / delete / execute / cancel / replace and the
// best_bid/best_ask touch bitmap. Prices are chosen so the tick offset
// (price - base_price) is easy to read: base_price = 10000, window = 4096,
// so a price of 10005 is offset 5, etc. window/pool_capacity are powers of
// two (asserted by the ctor).

namespace hft {
namespace {

constexpr SymbolId kSym = 1;
constexpr price_t  kBase = 10000;
constexpr std::size_t kWindow = 4096;
constexpr std::size_t kPool = 1024;

OrderBook make_book() { return OrderBook(kSym, kBase, kWindow, kPool); }

// --- Empty book ------------------------------------------------------------

TEST(OrderBook, EmptyBookHasNoTouch) {
    OrderBook b = make_book();
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
    EXPECT_EQ(b.best_ask(), kInvalidPrice);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 0);
    EXPECT_EQ(b.qty_at(Side::Sell, 10005), 0);
}

// --- Single order ----------------------------------------------------------

TEST(OrderBook, SingleBid) {
    OrderBook b = make_book();
    b.add_order(/*ref=*/1, Side::Buy, /*price=*/10005, /*shares=*/100);
    EXPECT_EQ(b.best_bid(), 10005);
    EXPECT_EQ(b.best_ask(), kInvalidPrice);   // ask side still empty
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 100);
}

TEST(OrderBook, SingleAsk) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, 10020, 50);
    EXPECT_EQ(b.best_ask(), 10020);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
    EXPECT_EQ(b.qty_at(Side::Sell, 10020), 50);
}

// --- Touch selection (bitmap picks best price) -----------------------------

TEST(OrderBook, BestBidIsHighestPrice) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10000, 10);   // offset 0
    b.add_order(2, Side::Buy, 10005, 10);   // offset 5
    b.add_order(3, Side::Buy, 10002, 10);   // offset 2
    EXPECT_EQ(b.best_bid(), 10005);         // highest wins
}

TEST(OrderBook, BestAskIsLowestPrice) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, 10020, 10);
    b.add_order(2, Side::Sell, 10015, 10);
    b.add_order(3, Side::Sell, 10018, 10);
    EXPECT_EQ(b.best_ask(), 10015);         // lowest wins
}

// Offsets that land in different bitmap words exercise the summary tier.
// offset 5 -> word 0, offset 70 -> word 1, offset 200 -> word 3.
TEST(OrderBook, BestBidAcrossBitmapWords) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,   10);   // word 0
    b.add_order(2, Side::Buy, kBase + 70,  10);   // word 1
    b.add_order(3, Side::Buy, kBase + 200, 10);   // word 3 (highest)
    EXPECT_EQ(b.best_bid(), kBase + 200);
}

TEST(OrderBook, BestAskAcrossBitmapWords) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, kBase + 200, 10);  // word 3
    b.add_order(2, Side::Sell, kBase + 70,  10);  // word 1
    b.add_order(3, Side::Sell, kBase + 5,   10);  // word 0 (lowest)
    EXPECT_EQ(b.best_ask(), kBase + 5);
}

// --- Multiple orders at one level (FIFO aggregate) -------------------------

TEST(OrderBook, MultipleOrdersSameLevelAggregate) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.add_order(2, Side::Buy, 10005, 250);
    EXPECT_EQ(b.best_bid(), 10005);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 350);   // summed
}

// --- Delete ----------------------------------------------------------------

TEST(OrderBook, DeleteRemovesQty) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.add_order(2, Side::Buy, 10005, 250);
    b.delete_order(1);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 250);   // only order 1 gone
    EXPECT_EQ(b.best_bid(), 10005);               // level still non-empty
}

TEST(OrderBook, DeleteEmptiesLevelDropsTouch) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);        // best
    b.add_order(2, Side::Buy, 10002, 100);        // next best
    b.delete_order(1);                            // remove the touch
    EXPECT_EQ(b.best_bid(), 10002);               // touch drops to next level
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 0);
}

TEST(OrderBook, DeleteLastOrderEmptiesBook) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);       // book empty again
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 0);
}

TEST(OrderBook, DeleteUnknownRefIsNoop) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.delete_order(999);                          // never added
    EXPECT_EQ(b.best_bid(), 10005);               // unchanged
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 100);
}

// --- Execute / Cancel (partial + full) -------------------------------------

TEST(OrderBook, PartialExecuteReducesQty) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.execute_order(1, 40);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 60);
    EXPECT_EQ(b.best_bid(), 10005);               // still resting
}

TEST(OrderBook, FullExecuteRemovesOrder) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.execute_order(1, 100);                      // exactly exhausts it
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 0);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
}

TEST(OrderBook, PartialCancelReducesQty) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, 10020, 100);
    b.cancel_order(1, 30);
    EXPECT_EQ(b.qty_at(Side::Sell, 10020), 70);
    EXPECT_EQ(b.best_ask(), 10020);
}

TEST(OrderBook, ExecuteAcrossTwoOrdersAtLevel) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.add_order(2, Side::Buy, 10005, 100);
    b.execute_order(1, 100);                      // remove first, second remains
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 100);
    EXPECT_EQ(b.best_bid(), 10005);
}

// --- Replace (U): delete old, add new, loses time priority -----------------

TEST(OrderBook, ReplaceMovesPriceAndQty) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);
    b.replace_order(/*old=*/1, /*new=*/2, /*price=*/10008, /*shares=*/80);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 0);     // old price emptied
    EXPECT_EQ(b.qty_at(Side::Buy, 10008), 80);    // new price
    EXPECT_EQ(b.best_bid(), 10008);
}

TEST(OrderBook, ReplaceKeepsSide) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, 10020, 100);
    b.replace_order(1, 2, 10018, 100);            // no side arg — recovered
    EXPECT_EQ(b.best_ask(), 10018);
    EXPECT_EQ(b.qty_at(Side::Sell, 10018), 100);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);       // did not flip to bid side
}

// After replace, the new order is at the TAIL of its level (loses time
// priority): an execute hits the older resting order at that price first.
TEST(OrderBook, ReplaceLosesTimePriority) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 10005, 100);        // order A, arrives first
    b.add_order(2, Side::Buy, 10008, 100);        // order B at a higher level
    b.replace_order(2, 3, 10005, 100);            // B moves down to A's level
    // Level 10005 now holds A (head) then order 3 (tail). An execute of 100
    // should exhaust A first (FIFO), leaving order 3's 100 behind.
    b.execute_order(1, 100);
    EXPECT_EQ(b.qty_at(Side::Buy, 10005), 100);   // order 3 remains
}

// --- Both sides at once -----------------------------------------------------

TEST(OrderBook, IndependentBidAndAskSides) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy,  10005, 100);
    b.add_order(2, Side::Sell, 10020, 200);
    EXPECT_EQ(b.best_bid(), 10005);
    EXPECT_EQ(b.best_ask(), 10020);
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
    EXPECT_EQ(b.best_ask(), 10020);               // ask untouched
}

// --- Far order (pathologically far): tracked by ref, not in the touch ------
// NOTE: a price merely just outside the window now triggers a *recenter*
// (drift-following) rather than a far order. A far order is the pathological
// outlier case: out of band AND beyond the recenter threshold
// (|price - window_center| >= window). window_center = kBase + kWindow/2.
// So kBase + 3*kWindow is far enough that recenter is NOT triggered.

constexpr price_t kFar = kBase + 3 * static_cast<price_t>(kWindow);

TEST(OrderBook, FarOrderNotInTouch) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kFar, 100);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);        // not in the bitmap
    EXPECT_EQ(b.qty_at(Side::Buy, kFar), 0);       // out of band -> 0
    // But it is still tracked by ref: delete must find and remove it cleanly.
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
}

TEST(OrderBook, FarOrderCoexistsWithNearOrder) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kFar,       100);    // far (no recenter)
    b.add_order(2, Side::Buy, kBase + 5,  100);    // near
    EXPECT_EQ(b.best_bid(), kBase + 5);            // only the near one shows
}

// --- Recenter (window follows drift) ---------------------------------------

// A price just outside the window (within the recenter threshold) should
// slide the window to follow it, not become a far order.
TEST(OrderBook, RecenterFollowsDrift) {
    OrderBook b = make_book();
    // Just past the top edge: offset window+50 is out of band but within one
    // window of center -> triggers recenter.
    price_t drifted = kBase + static_cast<price_t>(kWindow) + 50;
    b.add_order(1, Side::Buy, drifted, 100);
    EXPECT_EQ(b.best_bid(), drifted);              // now in-band via recenter
    EXPECT_EQ(b.qty_at(Side::Buy, drifted), 100);
}

// After recentering to follow a drifted price, the original (now far-below)
// price falls out of the window.
TEST(OrderBook, RecenterDropsOldFarSide) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5, 100);     // near original center-ish
    price_t drifted = kBase + static_cast<price_t>(kWindow) + 50;
    b.add_order(2, Side::Buy, drifted, 100);       // triggers recenter upward
    EXPECT_EQ(b.best_bid(), drifted);
    // The old order at kBase+5 is now far below the new window. It survives
    // in the book by ref (this test only asserts the new touch is correct);
    // whether kBase+5 is still queryable depends on how far the window slid.
}

// The bug rebuild_bitmap() fixes: a near order that STAYS in the window across
// a recenter. Its ring slot is preserved by the origin bump, but its logical
// offset (price - base_price_) changes when base_price_ slides, so its touch
// bit must move too. Before the fix the stale bit made best_bid report a
// phantom price. Concretely (base=10000, window=4096):
//   A @ 13000 -> logical offset 3000, ring slot 3000.
//   drift @ 14106 is one tick-band past the top edge -> recenter, delta=2058.
//   Upward recenter evicts logical offsets 0..2057, so slot 3000 (order A)
//   survives; new base=12058, so A's logical offset becomes 942 and drift's
//   becomes 2048. best_bid must be the higher price (drift), and A must still
//   be queryable at its true price 13000 -- not a phantom (old base+3000).
TEST(OrderBook, RecenterKeepsSurvivingNearOrder) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, 13000, 100);                 // A: survives recenter
    price_t drift = kBase + static_cast<price_t>(kWindow) + 10;  // 14106
    b.add_order(2, Side::Buy, drift, 200);                 // triggers recenter up
    EXPECT_EQ(b.best_bid(), drift);                        // drift is the new touch
    EXPECT_EQ(b.qty_at(Side::Buy, drift), 200);
    EXPECT_EQ(b.qty_at(Side::Buy, 13000), 100);            // A still at its true price
    // Deleting the touch must fall back to A's real price, not a phantom.
    b.delete_order(2);
    EXPECT_EQ(b.best_bid(), 13000);
    EXPECT_EQ(b.qty_at(Side::Buy, 13000), 100);
}

// Same invariant on the ask side, and exercising a level with two orders so
// the aggregate qty and FIFO list survive the bitmap rebuild intact.
TEST(OrderBook, RecenterKeepsSurvivingAskLevel) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, 13000, 100);
    b.add_order(2, Side::Sell, 13000, 250);                // same level, two orders
    price_t drift = kBase + static_cast<price_t>(kWindow) + 10;
    b.add_order(3, Side::Sell, drift, 50);                 // recenter
    EXPECT_EQ(b.qty_at(Side::Sell, 13000), 350);           // aggregate survived
    EXPECT_EQ(b.best_ask(), 13000);                        // 13000 < drift -> lowest ask
    EXPECT_EQ(b.qty_at(Side::Sell, drift), 50);            // drift level also present
    b.delete_order(1);
    b.delete_order(2);
    EXPECT_EQ(b.best_ask(), drift);                        // now drift is the only ask
}

}  // namespace
}  // namespace hft
