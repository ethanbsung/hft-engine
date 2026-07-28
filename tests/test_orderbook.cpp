#include <gtest/gtest.h>
#include "hft/orderbook.hpp"

// Tests for OrderBook: add / delete / execute / cancel / replace and the
// best_bid/best_ask touch bitmap. Prices are chosen so the tick offset
// (price - base_price) is easy to read: base_price = 10000, so a price of
// 10005 is offset 5, etc. window/pool_capacity are powers of two (asserted by
// the ctor).
//
// The book uses a FIXED price window: base_price is supplied at construction
// (in production, from the previous close; see book_set.cpp) and never moves.
// A price outside [base_price, base_price + window) is a "far" order — tracked
// by ref so its later delete/execute resolves, but contributing no price level
// and no touch bit. There is no recentring.
//
// The touch bitmap is three-tier over `window` bits:
//   bits_[512] (one bit per tick) -> mid_[8] (one bit per bits_ word)
//                                 -> summary (one bit per mid_ word)
// so a window of 32768 needs 512 words, 8 mid words, 8 summary bits.

namespace hft {
namespace {

constexpr SymbolId kSym = 1;
constexpr price_t  kBase = 10000;
constexpr std::size_t kWindow = 32768;
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

// --- Far orders: outside the fixed window, tracked by ref only -------------
// The window is [kBase, kBase + kWindow). Anything outside is "far": it holds
// a pool slot and a ref-index entry (so its later delete/execute resolves) but
// contributes no price level and no touch bit. In the real feed these are
// ITCH's $199,999.99 "no price" sentinel and penny stubs.

constexpr price_t kFarAbove = kBase + static_cast<price_t>(kWindow) + 1;
constexpr price_t kFarBelow = kBase - 1;

TEST(OrderBook, FarOrderAboveWindowNotInTouch) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,  100);
    b.add_order(2, Side::Buy, kFarAbove,  100);
    EXPECT_EQ(b.best_bid(), kBase + 5);              // far one does not become touch
    EXPECT_EQ(b.qty_at(Side::Buy, kFarAbove), 0);    // out of band -> 0
    b.delete_order(2);                               // still resolvable by ref
    EXPECT_EQ(b.best_bid(), kBase + 5);
}

TEST(OrderBook, FarOrderBelowWindowNotInTouch) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,  100);
    b.add_order(2, Side::Buy, kFarBelow,  100);      // one tick below base
    EXPECT_EQ(b.best_bid(), kBase + 5);
    EXPECT_EQ(b.qty_at(Side::Buy, kFarBelow), 0);
    b.delete_order(2);
    EXPECT_EQ(b.best_bid(), kBase + 5);
}

// A far order must not leak a pool slot: deleting it frees the slot, and the
// book keeps working afterwards.
TEST(OrderBook, FarOrderDeleteFreesSlot) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kFarAbove, 100);
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);          // nothing resting
    b.add_order(2, Side::Buy, kBase + 7, 100);       // book still usable
    EXPECT_EQ(b.best_bid(), kBase + 7);
}

// Execute against a far order reduces it by ref without touching any level.
TEST(OrderBook, FarOrderExecuteResolvesByRef) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,  100);
    b.add_order(2, Side::Buy, kFarAbove,  100);
    b.execute_order(2, 100);                         // fully exhaust the far order
    EXPECT_EQ(b.best_bid(), kBase + 5);              // near book unaffected
    EXPECT_EQ(b.qty_at(Side::Buy, kBase + 5), 100);
}

// The window edges themselves are IN band; one tick beyond is not.
TEST(OrderBook, WindowBoundariesAreInclusiveLowExclusiveHigh) {
    OrderBook b = make_book();
    const price_t top = kBase + static_cast<price_t>(kWindow) - 1;
    b.add_order(1, Side::Buy, kBase, 100);           // lowest in-band tick
    EXPECT_EQ(b.best_bid(), kBase);
    b.add_order(2, Side::Buy, top, 100);             // highest in-band tick
    EXPECT_EQ(b.best_bid(), top);
    EXPECT_EQ(b.qty_at(Side::Buy, top), 100);
}

// --- Three-tier bitmap ------------------------------------------------------
// bits_[512] -> mid_[8] -> summary. Offsets are chosen to land in different
// words at each tier so a missing tier-update shows up as a wrong touch.
//   offset      -> bits word (off>>6), mid word (word>>6), summary bit
//   5           -> 0,   0, 0
//   4096        -> 64,  1, 1
//   20480       -> 320, 5, 5
//   32767       -> 511, 7, 7   (top of window)

TEST(OrderBook, BestBidAcrossMidWords) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,     10);    // mid word 0
    b.add_order(2, Side::Buy, kBase + 4096,  10);    // mid word 1
    b.add_order(3, Side::Buy, kBase + 20480, 10);    // mid word 5 (highest)
    EXPECT_EQ(b.best_bid(), kBase + 20480);
}

TEST(OrderBook, BestAskAcrossMidWords) {
    OrderBook b = make_book();
    b.add_order(1, Side::Sell, kBase + 20480, 10);   // mid word 5
    b.add_order(2, Side::Sell, kBase + 4096,  10);   // mid word 1
    b.add_order(3, Side::Sell, kBase + 5,     10);   // mid word 0 (lowest)
    EXPECT_EQ(b.best_ask(), kBase + 5);
}

// Clearing must cascade bits -> mid -> summary, but only when the tier above
// is genuinely empty. Emptying the top level should fall back to the next one
// down, several mid-words away.
TEST(OrderBook, ClearCascadeFallsBackAcrossMidWords) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 5,     10);
    b.add_order(2, Side::Buy, kBase + 20480, 10);
    EXPECT_EQ(b.best_bid(), kBase + 20480);
    b.delete_order(2);                               // clears bits/mid/summary bit
    EXPECT_EQ(b.best_bid(), kBase + 5);              // falls back, not phantom
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);          // all tiers clear
}

// A mid-word bit must NOT be cleared while another level in the same bits word
// is still occupied. Offsets 20480 and 20481 share bits word 320.
TEST(OrderBook, ClearKeepsMidBitWhenWordStillOccupied) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 20480, 10);
    b.add_order(2, Side::Buy, kBase + 20481, 10);    // same bits word
    EXPECT_EQ(b.best_bid(), kBase + 20481);
    b.delete_order(2);
    EXPECT_EQ(b.best_bid(), kBase + 20480);          // sibling still visible
}

// Same, one tier up: offsets 4096 and 8192 are different bits words (64, 128)
// but both in mid word 1. Emptying one must leave the other reachable.
TEST(OrderBook, ClearKeepsSummaryBitWhenMidStillOccupied) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy, kBase + 4096, 10);     // bits word 64,  mid 1
    b.add_order(2, Side::Buy, kBase + 8192, 10);     // bits word 128, mid 1
    EXPECT_EQ(b.best_bid(), kBase + 8192);
    b.delete_order(2);
    EXPECT_EQ(b.best_bid(), kBase + 4096);           // still found via mid word 1
}

// Top-of-window offset exercises the highest index at every tier.
TEST(OrderBook, TouchAtTopOfWindow) {
    OrderBook b = make_book();
    const price_t top = kBase + static_cast<price_t>(kWindow) - 1;  // offset 32767
    b.add_order(1, Side::Buy,  top, 10);
    b.add_order(2, Side::Sell, top, 10);
    EXPECT_EQ(b.best_bid(), top);
    EXPECT_EQ(b.best_ask(), top);
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
    EXPECT_EQ(b.best_ask(), top);                    // ask side independent
}

// Bid and ask bitmaps are separate structures: filling one must not perturb
// the other's tiers.
TEST(OrderBook, BidAndAskBitmapsAreIndependent) {
    OrderBook b = make_book();
    b.add_order(1, Side::Buy,  kBase + 100,   10);
    b.add_order(2, Side::Sell, kBase + 20480, 10);
    EXPECT_EQ(b.best_bid(), kBase + 100);
    EXPECT_EQ(b.best_ask(), kBase + 20480);
    b.delete_order(1);
    EXPECT_EQ(b.best_bid(), kInvalidPrice);
    EXPECT_EQ(b.best_ask(), kBase + 20480);          // ask untouched
}

}  // namespace
}  // namespace hft
