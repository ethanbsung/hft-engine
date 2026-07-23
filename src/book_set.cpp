#include "hft/book_set.hpp"
#include "hft/orderbook.hpp"
#include <unordered_set>

namespace {
const std::unordered_set<std::string_view> kWatchlist{
    "SPY     ", "QQQ     ", "MSFT    ", "AAPL    "
    , "TSLA    ", "GOOGL   ", "AMZN    "
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
    if (kWatchlist.count(sym) == 0) return;
    if (by_locate_[locate] != nullptr) return;

    storage_.emplace_back(locate, 0, window_, pool_);
    by_locate_[locate] = &storage_.back();
}

}