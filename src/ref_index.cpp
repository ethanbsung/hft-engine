#include "hft/ref_index.hpp"
#include <cassert>

namespace hft {

RefIndex::RefIndex(std::size_t capacity_pow2) : slots_(capacity_pow2, Slot{0, 0}), mask_(capacity_pow2 - 1) {
    assert(capacity_pow2 != 0 && (capacity_pow2 & (capacity_pow2 - 1)) == 0
       && "capacity must be a non-zero power of two");
}

void RefIndex::insert(order_ref_t ref, uint32_t idx) noexcept {
    std::size_t i = mix(ref) & mask_;
    while (true) {
        if (slots_[i].ref == 0) {
            slots_[i].ref = ref;
            slots_[i].idx = idx;
            return;
        }
        i = (i + 1) & mask_;
    }
}

uint32_t RefIndex::find(order_ref_t ref) const noexcept {
    std::size_t i = mix(ref) & mask_;
    while (true) {
        if (ref == slots_[i].ref) {
            return slots_[i].idx;
        } else if (slots_[i].ref == 0) {
            return kNullIdx;
        }
        i = (i + 1) & mask_;
    }
}

void RefIndex::erase(order_ref_t ref) noexcept {
    std::size_t hole = mix(ref) & mask_;
    while (true) {
        if (slots_[hole].ref == ref) {
            break;
        } else if (slots_[hole].ref == 0) {
            return;
        }
        hole = (hole + 1) & mask_;
    }

    slots_[hole].ref = 0;
    std::size_t j = (hole + 1) & mask_;
    auto dist = [&](std::size_t x) { return (x - hole) & mask_; };
    while (true) {
        if (slots_[j].ref == 0) return;
        std::size_t ideal = mix(slots_[j].ref) & mask_;
        if (dist(ideal) <= dist(j)) {
            slots_[hole] = slots_[j];
            slots_[j].ref = 0;
            hole = j;
        }
        j = (j + 1) & mask_;
    }
}



}