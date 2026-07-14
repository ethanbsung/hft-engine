#include <gtest/gtest.h>

#include "hft/types.hpp"

// Smoke test: proves the gtest harness compiles, links, and runs.
// Replace/extend with real tests as you build each module
// (test_order_book.cpp, test_matching_engine.cpp, ...).
TEST(Smoke, BuildAndLink) {
    hft::price_t p = 100;
    hft::qty_t q = 5;
    EXPECT_EQ(p * q, 500);
}
