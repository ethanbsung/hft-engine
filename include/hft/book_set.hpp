#pragma once

#include <deque>
#include <vector>
#include <string_view>
#include "orderbook.hpp"

namespace hft {

class BookSet {
public:
    BookSet(std::size_t window, std::size_t pool_capacity);

    OrderBook* get(uint16_t locate) noexcept;
    void on_directory(uint16_t locate, std::string_view sym);
private:
    std::deque<OrderBook> storage_;
    std::vector<OrderBook*> by_locate_;
    std::size_t window_;
    std::size_t pool_;
};

}