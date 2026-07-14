#include <gtest/gtest.h>

#include "hft/types.hpp"
#include "hft/feed_handler.hpp"
#include <vector>
#include <string>

/*
Plan:

1. Test that constructs a SymbolTable, calls
   intern("BTC-USD", 0.01, 0.00000001, 100), constructs a Handler

2. Read line 2 of tests/fixtures/btc_usd_l2.jsonl (a real captured
   "update" event, two price levels, both bid side):

   {"channel":"l2_data","timestamp":"2026-07-06T04:17:46.374702991Z","sequence_num":1,
    "events":[{"type":"update","product_id":"BTC-USD","updates":[
      {"side":"bid","event_time":"2026-07-06T04:17:46.105221Z","price_level":"63211.05","new_quantity":"0"},
      {"side":"bid","event_time":"2026-07-06T04:17:46.105221Z","price_level":"55339.62","new_quantity":"0.0139668"}
    ]}]}

   Expected BookDelta values by hand (tick_size=0.01, scale=100,
   qty_increment=0.00000001), passing recv_ts = 12345:

   deltas[0]: side = Side::Buy
              price   = 6321105   (63211.05 * 100)
              new_qty = 0         (0 / 0.00000001)
              symbol  = 0         (first symbol interned)
              recv_ts = 12345
              exchange_ts = 0     (timestamp parser not written yet)

   deltas[1]: side = Side::Buy
              price   = 5533962   (55339.62 * 100)
              new_qty = 1396680   (0.0139668 / 0.00000001)
              symbol  = 0
              recv_ts = 12345
              exchange_ts = 0

3. Call parse_l2_message and assert deltas.size() == 2, then assert
   each field above.



Open question before writing the file-reading code: how to locate
tests/fixtures/btc_usd_l2.jsonl from the test binary's CWD (ctest/
gtest_discover_tests runs from the build dir, not tests/) — e.g. a
std::filesystem::path(__FILE__).parent_path() trick, vs. a hardcoded
absolute path for now.
*/

// TEST(TestSuiteName, TestName)



namespace hft {

TEST(Feed, SymbolTableIntern) {
    SymbolTable symbols;
    Handler h;
    std::string raw_json = R"({"channel":"l2_data","timestamp":"2026-07-06T04:17:46.374702991Z","sequence_num":1,
    "events":[{"type":"update","product_id":"BTC-USD","updates":[
      {"side":"bid","event_time":"2026-07-06T04:17:46.105221Z","price_level":"63211.05","new_quantity":"0"},
      {"side":"bid","event_time":"2026-07-06T04:17:46.105221Z","price_level":"55339.62","new_quantity":"0.0139668"}
    ]}]})";
    SymbolId id = symbols.intern("BTC-USD", 0.01, 0.00000001, 100);
    std::vector<BookDelta> testDeltas = h.parse_l2_message(raw_json, symbols, 12345);
    ASSERT_EQ(2, testDeltas.size());
    ASSERT_EQ(Side::Buy, testDeltas[0].side);
    ASSERT_EQ(6321105, testDeltas[0].price);
    ASSERT_EQ(0, testDeltas[0].new_qty);
    ASSERT_EQ(id, testDeltas[0].symbol);
    ASSERT_EQ(12345, testDeltas[0].recv_ts);
    ASSERT_EQ(0, testDeltas[0].exchange_ts);

    ASSERT_EQ(Side::Buy, testDeltas[1].side);
    ASSERT_EQ(5533962, testDeltas[1].price);
    ASSERT_EQ(1396680, testDeltas[1].new_qty);
    ASSERT_EQ(id, testDeltas[1].symbol);
    ASSERT_EQ(12345, testDeltas[1].recv_ts);
    ASSERT_EQ(0, testDeltas[1].exchange_ts);

}

/*
4. A second test: hardcode a minimal non-l2_data JSON string
   (e.g. {"channel":"subscriptions","events":[]}) and assert
   parse_l2_message on it returns an empty vector.
*/

TEST(Feed, FeedEmptyVector) {
    std::string raw_json = R"({"channel":"subscriptions","events":[]})";
    Handler h;
    SymbolTable symbols;
    std::vector<BookDelta> testDeltas = h.parse_l2_message(raw_json, symbols, 12345);
    ASSERT_EQ(0, testDeltas.size());
}

}

