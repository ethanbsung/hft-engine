#pragma once
#include "types.hpp"
#include "ref_index.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace hft {

inline constexpr uint32_t kNoLevel = UINT32_MAX;
inline constexpr price_t kInvalidPrice = INT64_MIN;

struct RestingOrder {
    price_t price = 0;
    qty_t shares = 0;
    uint32_t prev_idx = kNullIdx;
    uint32_t next_idx = kNullIdx;
    order_ref_t ref = 0;
    Side side = Side::Buy;
};

struct Level {
    qty_t total_qty = 0;
    uint32_t head_idx = kNullIdx;
    uint32_t tail_idx = kNullIdx;
    uint32_t order_count = 0;
};

class OrderBook {
public:
    OrderBook(SymbolId symbol, price_t base_price, std::size_t window, std::size_t pool_capacity);

    void add_order(order_ref_t ref, Side side, price_t price, qty_t shares) noexcept;
    void execute_order(order_ref_t ref, qty_t shares) noexcept;   // E: reduce by shares
    void cancel_order(order_ref_t ref, qty_t shares) noexcept;     // X: reduce by shares
    void delete_order(order_ref_t ref) noexcept;                   // D: remove
    void replace_order(order_ref_t old_ref, order_ref_t new_ref,
                       price_t price, qty_t shares) noexcept; // U
    void remove_at_slot(uint32_t slot);
    price_t best_bid() const noexcept;
    price_t best_ask() const noexcept;
    qty_t qty_at(Side side, price_t price) const noexcept;

private:
    void reduce(order_ref_t ref, qty_t shares) noexcept;
    uint32_t alloc_slot() noexcept;
    void free_slot(uint32_t) noexcept;
    uint32_t index_of(price_t) const noexcept;
    void recenter(price_t new_center) noexcept;

    SymbolId symbol_;
    price_t base_price_;
    std::vector<RestingOrder> pool_;
    uint32_t free_head_ = kNullIdx;
    uint64_t recenters_ = 0;
    uint64_t far_orders_ = 0;
    uint64_t pool_full_drops_ = 0;
    uint32_t ring_origin_ = 0;
    uint64_t not_found_ = 0;
    std::size_t mask_;
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    uint64_t bid_summary_ = 0;
    uint64_t bid_bits_[64] = {};
    uint64_t ask_summary_ = 0;
    uint64_t ask_bits_[64] = {};

    RefIndex ref_index_;
};

}