#pragma once

#include "types.hpp"
#include "orderbook.hpp"
#include <span>


namespace hft {

class Handler {
public:
void add_order(order_ref_t ref, Side side, price_t price, qty_t shares);
void delete_order(order_ref_t ref);
size_t decode(std::span<const std::byte> packet, nanos_t recv_ts, OrderBook& book) noexcept;

private:

};

}