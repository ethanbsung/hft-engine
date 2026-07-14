#pragma once
#include "types.hpp"
#include "ref_index.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace hft {

struct RestingOrder {
    order_ref_t ref;
    price_t price;
    qty_t shares;
    Side side;
    uint32_t prev_idx;
    uint32_t next_idx;
};

struct Level {
    uint32_t head_idx = kNullIdx;
    uint32_t tail_idx = kNullIdx;
    qty_t total_qty = 0;
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

    price_t best_bid() const noexcept;
    price_t best_ask() const noexcept;
    qty_t qty_at(Side side, price_t price) const noexcept;

private:
    uint32_t alloc_slot() noexcept;
    void free_slot(uint32_t) noexcept;
    std::size_t index_of(price_t) const noexcept;
    SymbolId symbol_;
    price_t base_price_;
    std::vector<RestingOrder> pool_;
    uint32_t free_head_ = kNullIdx;
    
    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    std::size_t best_bid_idx_;
    std::size_t best_ask_idx_;

    RefIndex ref_index_;
};

}