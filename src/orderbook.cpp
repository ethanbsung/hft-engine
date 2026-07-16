#include "hft/orderbook.hpp"
#include <cassert>

namespace hft {

OrderBook::OrderBook(SymbolId symbol, price_t base_price, std::size_t window, std::size_t pool_capacity)
    : ref_index_(pool_capacity * 2) {
    assert(window != 0 && (window & (window - 1)) == 0
       && "window must be a non-zero power of two");
    assert(pool_capacity != 0 && (pool_capacity & (pool_capacity - 1)) == 0
       && "pool_capacity must be a non-zero power of two");
    symbol_ = symbol;
    base_price_ = base_price;
    mask_ = window - 1;
    pool_.resize(pool_capacity);
    bid_levels_.resize(window);
    ask_levels_.resize(window);
    
    for (std::size_t i = 0; i < pool_.size() - 1; i++) {
        pool_[i].next_idx = static_cast<uint32_t>(i + 1);
    }
    pool_[pool_.size() - 1].next_idx = kNullIdx;

    free_head_ = 0;
}

uint32_t OrderBook::alloc_slot() noexcept {
    uint32_t head = free_head_;
    if (head == kNullIdx) {
        return kNullIdx;
    }

    free_head_ = pool_[head].next_idx;
    return head;
}

void OrderBook::free_slot(uint32_t idx) noexcept {
    pool_[idx].next_idx = free_head_;
    free_head_ = idx;
}

std::size_t OrderBook::index_of(price_t price) const noexcept {
    price_t offset = price - base_price_;

    if (offset < 0 || offset > static_cast<price_t>(mask_)) {
        return kNoLevel;
    }

    size_t idx = (offset + ring_origin_) & mask_;
    return idx;
}

void OrderBook::add_order(order_ref_t ref, Side side, price_t price, qty_t shares) noexcept {
    uint32_t slot = alloc_slot();
    if (slot == kNullIdx) {
        pool_[slot].prev_idx = kNullIdx;
        return;
    } 
   
    std::size_t idx = index_of(price);
    auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    auto& bits = (side == Side::Buy) ? bid_bits_ : ask_bits_;
    auto& summary = (side == Side::Buy) ? bid_summary_ : ask_summary_;

    

    uint32_t old_tail = levels[idx].tail_idx;

    pool_[old_tail].next_idx = slot;
    pool_[slot].prev_idx = old_tail;
    pool_[slot].ref = ref;
    pool_[slot].side = side;
    pool_[slot].price = price;
    pool_[slot].shares = shares;
    pool_[slot].next_idx = kNullIdx;

    

    levels[idx].tail_idx = slot;
    levels[idx].total_qty += shares;
    levels[idx].order_count++;
    
    
    

    
    
    

    ref_index_.insert(ref, slot);

}

}

