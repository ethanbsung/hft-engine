#include "hft/feed_handler.hpp"
#include <nlohmann/json.hpp>
#include <cmath>

namespace hft {

using json = nlohmann::json;

std::vector<BookDelta> Handler::parse_l2_message(std::string_view raw_json, SymbolTable& symbols, nanos_t recv_ts) {
    std::vector<BookDelta> deltas;
    json msg = json::parse(raw_json);

    if (msg["channel"] != "l2_data") {
        return deltas;
    }

    for (const auto& event : msg["events"]) {
        // intern must be called first so lookup doesn't throw
        SymbolId symbol = symbols.lookup(event["product_id"].get<std::string>());
        for (const auto& update : event["updates"]) {
            BookDelta book;
            book.symbol = symbol;
            book.side = (update["side"] == "bid") ? Side::Buy : Side::Sell;
            book.price = static_cast<price_t>(std::round(std::stod(update["price_level"].get<std::string>()) * symbols.scale(book.symbol)));
            book.exchange_ts = 0; // needs timestamp parser
            book.recv_ts = recv_ts;
            book.new_qty = static_cast<qty_t>(std::round(std::stod(update["new_quantity"].get<std::string>()) / symbols.qty_increment(book.symbol)));
            deltas.push_back(book);
        }
    }
    return deltas;
}

}