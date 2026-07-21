#include "hft/feed_handler.hpp"
#include "hft/orderbook.hpp"
#include <cstring>
#include <span>

namespace hft {

void Handler::decode_message(std::span<const std::byte> payload, OrderBook& book) noexcept {

}
    
}