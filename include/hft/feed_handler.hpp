#pragma once

#include "types.hpp"
#include "orderbook.hpp"
#include <span>
#include <cstring>
#include <cstdint>
#include <cassert>

namespace hft {

class Handler {
public:
    std::size_t decode(std::span<const std::byte> buffer, nanos_t recv_ts,
                       OrderBook& book) noexcept;
                       
private:
    static uint64_t load_be_u64(const std::byte* p, std::size_t off) {
        uint64_t v;
        std::memcpy(&v, p+off, 8);
        return __builtin_bswap64(v);
    }

    static uint16_t load_be_u16(const std::byte* p, std::size_t off) {
        uint16_t v;
        std::memcpy(&v, p+off, 2);
        return __builtin_bswap16(v);
    }
    static uint32_t load_be_u32(const std::byte* p, std::size_t off) {
        uint32_t v;
        std::memcpy(&v, p+off, 4);
        return __builtin_bswap32(v);
    }
    static uint64_t load_be_u48(const std::byte* p, std::size_t off)  {
        uint64_t v = 0;
        std::memcpy(reinterpret_cast<std::byte*>(&v) + 2, p+off, 6);
        return __builtin_bswap64(v);
    }
    static Side side_from_byte(std::byte b) {
        assert(b == std::byte{'B'} || b == std::byte{'S'});
        return (b == std::byte{'B'}) ? Side::Buy : Side::Sell;
    }

    void decode_message(std::span<const std::byte> payload, OrderBook& book) noexcept;
    void on_add    (const std::byte* p, OrderBook& book) noexcept;  // A, F
    void on_delete (const std::byte* p, OrderBook& book) noexcept;  // D
    void on_execute(const std::byte* p, OrderBook& book) noexcept;  // E
    void on_cancel (const std::byte* p, OrderBook& book) noexcept;  // X
    void on_replace(const std::byte* p, OrderBook& book) noexcept;  // U
    void on_stock_directory(const std::byte* p) noexcept; // R: find SPY, set target_locate_

    uint16_t target_locate_ = 0;
    bool locate_found_ = false;
    std::size_t messages_ = 0;
};

}