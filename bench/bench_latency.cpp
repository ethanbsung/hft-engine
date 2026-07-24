#include <fstream>
#include <cstdint>
#include <cstdio>
#include "hft/feed_handler.hpp"
#include "hft/book_set.hpp"
#include "hft/platform.hpp"
#include <vector>
#include <cstddef>
#include "bench_util.hpp"
#include <algorithm>
#include <cassert>

int main() {
    constexpr const char* kFixturePath = "tests/fixtures/itch_500m.bin";
    constexpr uint64_t N = 900'000;

    std::ifstream in(kFixturePath, std::ios::binary | std::ios::ate);
    if (!in) return 1;
    std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<std::byte> buf(n);
    in.read(reinterpret_cast<char*>(buf.data()), n);

    uint64_t freq = hft::platform::cycles_per_sec();
    assert(freq != 0 && "cycles_per_sec returned 0");
    hft::LatencySink sink;
    sink.samples.reserve(N);

    hft::BookSet coldBooks(4096, 1 << 16);
    hft::Handler coldHandler;
    std::size_t frames = coldHandler.decode<false>(buf, 0, coldBooks);

    hft::BookSet books(4096, 1 << 16);
    hft::Handler handler;
    frames = handler.decode<true>(buf, 0, books, &sink);
    std::sort(sink.samples.begin(), sink.samples.end());
    do_not_optimize(frames);
    do_not_optimize(handler.messages());

    if (sink.samples.empty()) {
        std::fprintf(stderr, "no samples\n");
        return 1;
    }

    std::size_t samplesSize = sink.samples.size();
    double min = hft::platform::cycles_to_ns(sink.samples[0], freq);
    double max = hft::platform::cycles_to_ns(sink.samples[samplesSize - 1], freq);
    double p50 = hft::platform::cycles_to_ns(sink.samples[samplesSize * 50 / 100], freq);
    double p99 = hft::platform::cycles_to_ns(sink.samples[samplesSize * 99 / 100], freq);
    double p999 = hft::platform::cycles_to_ns(sink.samples[samplesSize * 999 / 1000], freq);

    std::printf("[%s] per-message dispatch+apply latency: \nmin: %.2f ns \np50: %.2f ns \np99: %.2f ns \np99.9: %.2f ns \nmax: %.2f ns \n",
                hft::platform::platform_tag(), min, p50, p99, p999, max);

    return 0;
}
