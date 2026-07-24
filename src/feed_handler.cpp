#include "hft/feed_handler.hpp"
#include "hft/orderbook.hpp"
#include "hft/book_set.hpp"
#include "hft/platform.hpp"
#include "hft/latency_sink.hpp"
#include <cstring>
#include <span>
#include <cassert>
#include <array>
#include <cstdint>

namespace hft {

void Handler::on_add(const std::byte* p, OrderBook& book) noexcept {
    order_ref_t ref = load_be_u64(p, 11);
    Side side = side_from_byte(p[19]);
    qty_t shares = load_be_u32(p, 20);
    price_t price = load_be_u32(p, 32) / 100;
    book.add_order(ref, side, price, shares);
}

void Handler::on_delete(const std::byte* p, OrderBook& book) noexcept {
    order_ref_t ref = load_be_u64(p, 11);
    book.delete_order(ref);
}

void Handler::on_execute(const std::byte* p, OrderBook& book) noexcept {
    order_ref_t ref = load_be_u64(p, 11);
    qty_t executed_shares = load_be_u32(p, 19);
    book.execute_order(ref, executed_shares);
}

void Handler::on_cancel(const std::byte* p, OrderBook& book) noexcept {
    order_ref_t ref = load_be_u64(p, 11);
    qty_t canceled_shares = load_be_u32(p, 19);
    book.cancel_order(ref, canceled_shares);
}

void Handler::on_replace(const std::byte* p, OrderBook& book) noexcept {
    order_ref_t original_order_ref = load_be_u64(p, 11);
    order_ref_t new_order_ref = load_be_u64(p, 19);
    qty_t shares = load_be_u32(p, 27);
    price_t price = load_be_u32(p, 31) / 100;
    book.replace_order(original_order_ref, new_order_ref, price, shares);
}

namespace {
constexpr auto kMinLen = [] {
    std::array<uint8_t, 256> t{};
    t['A'] = 36; t['F'] = 40;
    t['E'] = 31; t['C'] = 36;
    t['X'] = 23; t['D'] = 19;
    t['U'] = 35; t['R'] = 39;
    return t;
}();
}

void Handler::on_stock_directory(const std::byte* p, BookSet& books) noexcept {
    uint16_t locate = load_be_u16(p, 1);
    std::string_view sym(reinterpret_cast<const char*>(p + 11), 8);
    books.on_directory(locate, sym);
}

template<bool Timing>
std::size_t Handler::decode(std::span<const std::byte> buffer, [[maybe_unused]] nanos_t recv_ts, BookSet& books, LatencySink* sink) noexcept {
    std::size_t cursor = 0;
    uint64_t count = 0;
    while (buffer.size() - cursor >= 2) {
        uint16_t len = load_be_u16(buffer.data(), cursor);
        if (len == 0) break;
        if (len > buffer.size() - cursor - 2) break;
        decode_message<Timing>(buffer.subspan(cursor + 2, len), books, sink);
        cursor += 2 + len;
        count++;
    }
    return count;
}

template<bool Timing>
void Handler::decode_message(std::span<const std::byte> payload, BookSet& books, LatencySink* sink) noexcept {
    assert(payload.size() >= 3);
    const std::byte* p = payload.data();
    char type = static_cast<char>(p[0]);
    assert(payload.size() >= kMinLen[static_cast<unsigned char>(type)]);
    if (type == 'R') {
        on_stock_directory(p, books);
        return;
    }

    uint16_t locate = load_be_u16(p, 1);
    OrderBook* b = books.get(locate);
    if (!b) return;

    auto apply = [&] {
        switch(type) {
            case 'A':
            case 'F':
                ++messages_;
                on_add(p, *b);
                break;
            case 'D':
                ++messages_;
                on_delete(p, *b);
                break;
            case 'C':
            case 'E':
                ++messages_;
                on_execute(p, *b);
                break;
            case 'X':
                ++messages_;
                on_cancel(p, *b);
                break;
            case 'U':
                ++messages_;
                on_replace(p, *b);
                break;
            case 'P':
                return;
            default:
                return;
        }
    };
    
    if constexpr (Timing) {
        uint64_t t0 = platform::read_cycles();
        apply();
        uint64_t t1 = platform::read_cycles();
        sink->record(t1 - t0);
    } else {
        apply();
    }
}

template std::size_t Handler::decode<true>(std::span<const std::byte>, nanos_t, BookSet&, LatencySink*) noexcept;
template std::size_t Handler::decode<false>(std::span<const std::byte>, nanos_t, BookSet&, LatencySink*) noexcept;
    
}



