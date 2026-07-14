#pragma once

#include "types.hpp"
#include <vector>
#include <cstdint>

namespace hft {

inline constexpr uint32_t kNullIdx = UINT32_MAX;

class RefIndex {
public:
    explicit RefIndex(std::size_t capacity_pow2); // e.g. 1<<20
    void insert(order_ref_t ref, uint32_t idx) noexcept;
    uint32_t find(order_ref_t ref) const noexcept;
    void erase(order_ref_t ref) noexcept;

    static inline uint64_t mix(uint64_t x) noexcept {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }

private:
    struct Slot {
        order_ref_t ref;    // key; kEmpty means unused
        uint32_t    idx;    // value: pool slot
    };
    std::vector<Slot> slots_;
    std::size_t       mask_;   // capacity - 1
};

}