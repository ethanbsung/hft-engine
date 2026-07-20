#include "hft/orderbook.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>

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

uint32_t OrderBook::index_of(price_t price) const noexcept {
    price_t offset = price - base_price_;

    if (offset < 0 || offset > static_cast<price_t>(mask_)) {
        return kNoLevel;
    }

    uint32_t idx = (offset + ring_origin_) & mask_;
    return idx;
}

void OrderBook::add_order(order_ref_t ref, Side side, price_t price, qty_t shares) noexcept {
    uint32_t slot = alloc_slot();
    if (slot == kNullIdx) {
        pool_full_drops_++;
        return;
    } 

    pool_[slot].ref = ref;
    pool_[slot].side = side;
    pool_[slot].price = price;
    pool_[slot].shares = shares;
   
    uint32_t idx = index_of(price);
    std::size_t offset = static_cast<std::size_t>(price - base_price_);
    auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    auto& bits = (side == Side::Buy) ? bid_bits_ : ask_bits_;
    auto& summary = (side == Side::Buy) ? bid_summary_ : ask_summary_;

    if (idx == kNoLevel) {
        far_orders_++;
        ref_index_.insert(ref, slot);
        pool_[slot].prev_idx = kNullIdx;
        pool_[slot].next_idx = kNullIdx;
        return;
    }

    if (levels[idx].head_idx == kNullIdx) {
        levels[idx].head_idx = slot;
        levels[idx].tail_idx = slot;
        pool_[slot].prev_idx = kNullIdx;
        pool_[slot].next_idx = kNullIdx;

        std::size_t word = offset >> 6;
        std::size_t bit = offset & 63;
        bits[word] |= (1ULL << bit);
        summary |= (1ULL << word);
    } else {
        uint32_t old_tail = levels[idx].tail_idx;
        pool_[old_tail].next_idx = slot;
        pool_[slot].prev_idx = old_tail;
        pool_[slot].next_idx = kNullIdx;
        levels[idx].tail_idx = slot;
    }

    levels[idx].total_qty += shares;
    levels[idx].order_count++;

    ref_index_.insert(ref, slot);

}

void OrderBook::remove_at_slot(uint32_t slot) {
    price_t price = pool_[slot].price;
    Side side = pool_[slot].side;
    order_ref_t ref = pool_[slot].ref;
    uint32_t idx = index_of(price);

    if (idx == kNoLevel) {
        free_slot(slot);
        ref_index_.erase(ref);
        far_orders_--;
        return;
    }

    auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    auto& bits = (side == Side::Buy) ? bid_bits_ : ask_bits_;
    auto& summary = (side == Side::Buy) ? bid_summary_ : ask_summary_;

    uint32_t prev = pool_[slot].prev_idx;
    uint32_t next = pool_[slot].next_idx;
    if (prev != kNullIdx) {
        pool_[prev].next_idx = next;
    } else {
        levels[idx].head_idx = next;
    }

    if (next != kNullIdx) {
         pool_[next].prev_idx = prev;
    } else {
        levels[idx].tail_idx = prev;
    }

    levels[idx].order_count--;
    levels[idx].total_qty -= pool_[slot].shares;

    if (levels[idx].order_count == 0) {
        std::size_t offset = static_cast<std::size_t>(price - base_price_);
        std::size_t word = offset >> 6;
        std::size_t bit = offset & 63;
        bits[word] &= ~(1ULL << bit);
        if (bits[word] == 0) summary &= ~(1ULL << word);
    }

    free_slot(slot);
    ref_index_.erase(ref);
}

void OrderBook::delete_order(order_ref_t ref) noexcept {
    uint32_t slot = ref_index_.find(ref);
    if (slot == kNullIdx) {
        not_found_++;
        return;
    }

    remove_at_slot(slot);
}

void OrderBook::reduce(order_ref_t ref, qty_t shares) noexcept {
    uint32_t slot = ref_index_.find(ref);
    if (slot == kNullIdx) {
        not_found_++;
        return;
    }

    uint32_t idx = index_of(pool_[slot].price);
    auto& levels = (pool_[slot].side == Side::Buy) ? bid_levels_ : ask_levels_;

    if (pool_[slot].shares > shares) {
        pool_[slot].shares -= shares;
        if (idx != kNoLevel) {
            levels[idx].total_qty -= shares;
        }
        return;
    } else {
        remove_at_slot(slot);
    }
    
}

void OrderBook::execute_order(order_ref_t ref, qty_t shares) noexcept {
    reduce(ref, shares);
}

void OrderBook::cancel_order(order_ref_t ref, qty_t shares) noexcept {
    reduce(ref, shares);
}

void OrderBook::replace_order(order_ref_t old_ref, order_ref_t new_ref, price_t price, qty_t shares) noexcept {
    uint32_t slot = ref_index_.find(old_ref);
    if (slot == kNullIdx) {
        not_found_++;
        return;
    }
    Side side = pool_[slot].side;
    delete_order(old_ref);
    add_order(new_ref, side, price, shares);
}

qty_t OrderBook::qty_at(Side side, price_t price) const noexcept {
    auto& levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    uint32_t idx = index_of(price);
    
    if (idx != kNoLevel) {
        return levels[idx].total_qty;
    }

    return 0;
}

price_t OrderBook::best_bid() const noexcept {
    if (bid_summary_ == 0) return kInvalidPrice;
    std::size_t word = 63 - __builtin_clzll(bid_summary_);
    std::size_t bit = 63 - __builtin_clzll(bid_bits_[word]);
    std::size_t offset = word * 64 + bit;
    return base_price_ + offset;
}
    
price_t OrderBook::best_ask() const noexcept {
    if (ask_summary_ == 0) return kInvalidPrice;
    std::size_t word = __builtin_ctzll(ask_summary_);
    std::size_t bit = __builtin_ctzll(ask_bits_[word]);
    std::size_t offset = word * 64 + bit;
    return base_price_ + offset;
}

void OrderBook::recenter(price_t new_center) noexcept {
    price_t new_base = new_center - (mask_ + 1) / 2;
    price_t delta = new_base - base_price_;
    if (delta == 0) return;

    if (abs(delta) >= static_cast<price_t>(mask_ + 1)) {
        std::fill(bid_levels_.begin(), bid_levels_.end(), Level{});
        std::fill(ask_levels_.begin(), ask_levels_.end(), Level{});
        std::fill(std::begin(bid_bits_), std::end(bid_bits_), 0ULL);
        std::fill(std::begin(ask_bits_), std::end(ask_bits_), 0ULL);
        bid_summary_ = 0;
        ask_summary_ = 0;
    } else {
        if (delta > 0) {
            for (uint32_t o = 0; o < static_cast<uint32_t>(delta); o++) {
                uint32_t slot = (ring_origin_ + o) & mask_;
                assert(bid_levels_[slot].order_count == 0 && ask_levels_[slot].order_count == 0);

                bid_levels_[slot] = Level{};
                ask_levels_[slot] = Level{};

                uint32_t word = o >> 6, bit = o & 63;
                bid_bits_[word] &= ~(1ULL << bit);
                if (bid_bits_[word] == 0) bid_summary_ &= ~(1ULL << word);
                ask_bits_[word] &= ~(1ULL << bit);
                if (ask_bits_[word] == 0) ask_summary_ &= ~(1ULL << word);
            }
        } else {
            
        }
    }

    base_price_ += delta;
    ring_origin_ = (ring_origin_ + delta + (mask_ + 1)) & mask_;
    recenters_++;
}

}

