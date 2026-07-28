#include <gtest/gtest.h>
#include "hft/ref_index.hpp"

#include <random>
#include <unordered_map>
#include <vector>

// RefIndex is an open-addressed, linear-probing ref -> pool-slot map with
// backward-shift deletion. The delicate part is erase(): when a slot is
// vacated, entries after it that probed past that bucket must be shifted back,
// or find() will hit the new empty slot and stop early, orphaning them.
//
// These tests pin that invariant. The collision tests deliberately construct
// refs that hash to the SAME bucket, because the bug only appears when a probe
// chain is longer than one.

namespace hft {
namespace {

constexpr std::size_t kCap = 256;

// Find `count` distinct refs whose mix() lands in the same bucket, so they
// form one probe chain.
std::vector<order_ref_t> colliding_refs(std::size_t cap, int count) {
    const std::size_t mask = cap - 1;
    std::vector<order_ref_t> out;
    order_ref_t first = 1;
    const std::size_t target = RefIndex::mix(first) & mask;
    out.push_back(first);
    for (order_ref_t r = 2; r < 2'000'000 && static_cast<int>(out.size()) < count; ++r) {
        if ((RefIndex::mix(r) & mask) == target) out.push_back(r);
    }
    return out;
}

TEST(Ref, BasicContract) {
    RefIndex m(kCap);
    m.insert(1, 42);
    EXPECT_EQ(m.find(1), 42u);
}

TEST(Ref, MultipleKeys) {
    RefIndex m(kCap);
    for (order_ref_t r = 1; r <= 50; ++r) m.insert(r, static_cast<uint32_t>(r * 10));
    for (order_ref_t r = 1; r <= 50; ++r) EXPECT_EQ(m.find(r), r * 10) << "ref " << r;
}

TEST(Ref, MissingKeyReturnsNull) {
    RefIndex m(kCap);
    m.insert(1, 42);
    EXPECT_EQ(m.find(999), kNullIdx);
}

TEST(Ref, EraseRemovesOnlyTarget) {
    RefIndex m(kCap);
    m.insert(1, 10);
    m.insert(2, 20);
    m.erase(1);
    EXPECT_EQ(m.find(1), kNullIdx);
    EXPECT_EQ(m.find(2), 20u);
}

TEST(Ref, EraseMissingKeyIsNoop) {
    RefIndex m(kCap);
    m.insert(1, 10);
    m.erase(999);
    EXPECT_EQ(m.find(1), 10u);
}

TEST(Ref, ReinsertAfterErase) {
    RefIndex m(kCap);
    m.insert(1, 10);
    m.erase(1);
    m.insert(1, 77);
    EXPECT_EQ(m.find(1), 77u);
}

// --- Probe-chain integrity across erase -------------------------------------
// The cases the backward-shift logic must get right. Each uses refs that all
// hash to one bucket, so they occupy consecutive slots in a single chain.

TEST(Ref, EraseHeadOfCollisionChainKeepsTail) {
    const auto refs = colliding_refs(kCap, 3);
    ASSERT_EQ(refs.size(), 3u);
    RefIndex m(kCap);
    m.insert(refs[0], 11);
    m.insert(refs[1], 22);
    m.insert(refs[2], 33);

    m.erase(refs[0]);   // vacate the first slot in the chain

    EXPECT_EQ(m.find(refs[0]), kNullIdx);
    EXPECT_EQ(m.find(refs[1]), 22u) << "second in chain lost after erasing head";
    EXPECT_EQ(m.find(refs[2]), 33u) << "third in chain lost after erasing head";
}

TEST(Ref, EraseMiddleOfCollisionChainKeepsTail) {
    const auto refs = colliding_refs(kCap, 3);
    ASSERT_EQ(refs.size(), 3u);
    RefIndex m(kCap);
    m.insert(refs[0], 11);
    m.insert(refs[1], 22);
    m.insert(refs[2], 33);

    m.erase(refs[1]);

    EXPECT_EQ(m.find(refs[0]), 11u);
    EXPECT_EQ(m.find(refs[1]), kNullIdx);
    EXPECT_EQ(m.find(refs[2]), 33u) << "tail lost after erasing middle";
}

TEST(Ref, EraseHeadOfLongCollisionChainKeepsAll) {
    const auto refs = colliding_refs(kCap, 6);
    ASSERT_EQ(refs.size(), 6u);
    RefIndex m(kCap);
    for (std::size_t i = 0; i < refs.size(); ++i)
        m.insert(refs[i], static_cast<uint32_t>(100 + i));

    m.erase(refs[0]);

    EXPECT_EQ(m.find(refs[0]), kNullIdx);
    for (std::size_t i = 1; i < refs.size(); ++i)
        EXPECT_EQ(m.find(refs[i]), 100u + i) << "chain entry " << i << " lost";
}

// Erase every member of a chain one at a time; the survivors must stay
// reachable at each step.
TEST(Ref, EraseChainSequentiallyKeepsSurvivors) {
    const auto refs = colliding_refs(kCap, 5);
    ASSERT_EQ(refs.size(), 5u);
    RefIndex m(kCap);
    for (std::size_t i = 0; i < refs.size(); ++i)
        m.insert(refs[i], static_cast<uint32_t>(200 + i));

    for (std::size_t erased = 0; erased < refs.size(); ++erased) {
        m.erase(refs[erased]);
        for (std::size_t i = 0; i <= erased; ++i)
            EXPECT_EQ(m.find(refs[i]), kNullIdx) << "ref " << i << " should be gone";
        for (std::size_t i = erased + 1; i < refs.size(); ++i)
            EXPECT_EQ(m.find(refs[i]), 200u + i)
                << "survivor " << i << " lost after erasing " << erased;
    }
}

// --- Differential fuzz against a reference model ----------------------------
// The unit cases above pin known shapes; this catches shapes we did not think
// of. Ref space is deliberately small so probe chains form constantly.

TEST(Ref, DifferentialFuzzAgainstMap) {
    RefIndex m(kCap);
    std::unordered_map<order_ref_t, uint32_t> model;
    std::vector<order_ref_t> live;
    std::mt19937_64 rng(20260728);

    for (int iter = 0; iter < 20000; ++iter) {
        const bool do_insert = (rng() % 100) < 60 && live.size() < kCap / 2;
        if (do_insert) {
            const order_ref_t r = (rng() % 400) + 1;   // dense -> collisions
            if (model.count(r)) continue;
            const uint32_t idx = static_cast<uint32_t>(rng() % 100000);
            m.insert(r, idx);
            model[r] = idx;
            live.push_back(r);
        } else if (!live.empty()) {
            const std::size_t k = rng() % live.size();
            const order_ref_t r = live[k];
            m.erase(r);
            model.erase(r);
            live[k] = live.back();
            live.pop_back();
        }

        // Every live ref must still resolve to its slot.
        for (const auto& kv : model) {
            ASSERT_EQ(m.find(kv.first), kv.second)
                << "iter " << iter << " ref " << kv.first
                << " live=" << model.size();
        }
    }
}

}  // namespace
}  // namespace hft
