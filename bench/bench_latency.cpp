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

    double p50s[5];
    double p99s[5];
    double p999s[5];

    for (size_t i = 0; i < 5; ++i) {
        sink.samples.clear();
        hft::BookSet books(4096, 1 << 16);
        hft::Handler handler;
        frames = handler.decode<true>(buf, 0, books, &sink);
        do_not_optimize(frames);
        do_not_optimize(handler.messages());
        std::sort(sink.samples.begin(), sink.samples.end());
        uint64_t sz = sink.samples.size();
        assert(sz > 0 && "size of samples must be > 0");
        p50s[i] = hft::platform::cycles_to_ns(sink.samples[sz * 50 / 100], freq);
        p99s[i] = hft::platform::cycles_to_ns(sink.samples[sz * 99 / 100], freq);
        p999s[i] = hft::platform::cycles_to_ns(sink.samples[sz * 999 / 1000], freq);
    } 

    std::sort(p50s, p50s + 5);
    double p50_median = p50s[2];
    double p50_lo = p50s[0];
    double p50_hi = p50s[4];

    std::sort(p99s, p99s + 5);
    double p99_median = p99s[2];
    double p99_lo = p99s[0];
    double p99_hi = p99s[4];

    std::sort(p999s, p999s + 5);
    double p999_median = p999s[2];
    double p999_lo = p999s[0];
    double p999_hi = p999s[4];

    std::printf("[%s] per-message dispatch+apply latency (median of 5 runs, range across runs):\n"
                "  p50:   %7.2f ns  (runs %.2f-%.2f)\n"
                "  p99:   %7.2f ns  (runs %.2f-%.2f)\n"
                "  p99.9: %7.2f ns  (runs %.2f-%.2f)\n",
                hft::platform::platform_tag(),
                p50_median,  p50_lo,  p50_hi,
                p99_median,  p99_lo,  p99_hi,
                p999_median, p999_lo, p999_hi);

    return 0;
}
