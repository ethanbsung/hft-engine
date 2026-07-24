#pragma once

#include <vector>

namespace hft {

struct LatencySink {
    std::vector<uint32_t> samples;
    void record(uint64_t delta) {
        samples.push_back(static_cast<uint32_t>(delta));
    }
};

}