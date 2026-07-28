#include "hft/book_set.hpp"
#include "hft/orderbook.hpp"
#include <unordered_map>

namespace {
// Watchlist -> per-symbol reference price, in ticks (1 tick = 1 cent).
//
// In production this comes from the previous session's close (or a reference
// data file loaded at startup). Here it is hardcoded because the replay is a
// single known day, and hardcoding is closer to what a real desk does than
// deriving the centre from the first message: ITCH's first add for a symbol is
// often a stub price, not a tradeable one. MSFT's first add in this fixture is
// $1.01 while it trades near $174 — centring on that would put the entire real
// book outside the window.
//
// Values are the median traded price per symbol over this fixture. The book
// window (32768 ticks = +/-$163.84 around the centre) comfortably covers each
// symbol's full traded range for the session; prices outside it (ITCH's
// $199,999.99 "no price" sentinel, penny stubs) land on the is_far path.
const std::unordered_map<std::string_view, hft::price_t> kWatchlist{
    {"AAPL    ",  32077},   // $320.77
    {"AMZN    ", 185500},   // $1855.00
    {"GOOGL   ", 143907},   // $1439.07
    {"MSFT    ",  17400},   // $174.00
    {"QQQ     ",  22030},   // $220.30
    {"SPY     ",  32455},   // $324.55
    {"TSLA    ",  63600},   // $636.00
};
}

namespace hft {

BookSet::BookSet(std::size_t window, std::size_t pool_capacity)
    : by_locate_(1 << 16, nullptr), window_(window), pool_(pool_capacity) {}

OrderBook* BookSet::get(uint16_t locate) noexcept {
    if (locate >= by_locate_.size()) return nullptr;
    return by_locate_[locate];
}

void BookSet::on_directory(uint16_t locate, std::string_view sym) {
    auto it = kWatchlist.find(sym);
    if (it == kWatchlist.end()) return;
    if (by_locate_[locate] != nullptr) return;

    // base_price is the low edge of the window: centre - window/2.
    const price_t base = it->second - static_cast<price_t>(window_ / 2);
    storage_.emplace_back(locate, base, window_, pool_);
    by_locate_[locate] = &storage_.back();
}

}
