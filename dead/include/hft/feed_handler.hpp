#pragma once

#include "types.hpp"
#include <string_view>
#include <vector>

namespace hft {

class Handler {
public:
    std::vector<BookDelta> parse_l2_message(std::string_view raw_json, SymbolTable& symbols, nanos_t recv_ts);

private:
    
};

}