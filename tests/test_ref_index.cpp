#include <gtest/gtest.h>
#include "hft/ref_index.hpp"

namespace hft {

TEST(Ref, BasicContract) {
    RefIndex hashMap(16);
    order_ref_t ref = 1;
    uint32_t idx = 42;
    hashMap.insert(ref, idx);
    ASSERT_EQ(hashMap.find(ref), idx);
}

TEST(Ref, MultipleKeys) {

}

}