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
    if (freq == 0) {
        fprintf(stderr, "freq == 0\n");
        return 1;
    }
    hft::LatencySink sink;
    sink.samples.reserve(N);

    hft::BookSet coldBooks(32'768, 1 << 16);
    hft::Handler coldHandler;
    std::size_t frames = coldHandler.decode<false>(buf, 0, coldBooks);

    double p50s[5];
    double p99s[5];
    double p999s[5];

    for (size_t i = 0; i < 5; ++i) {
        sink.samples.clear();
        hft::BookSet books(32'768, 1 << 16);
        hft::Handler handler;
        frames = handler.decode<true>(buf, 0, books, &sink);
        do_not_optimize(frames);
        do_not_optimize(handler.messages());
        std::sort(sink.samples.begin(), sink.samples.end());
        uint32_t tail_ticks = static_cast<uint32_t>(5000.0 * freq / 1e9);
        auto it = std::lower_bound(sink.samples.begin(), sink.samples.end(), tail_ticks);
        std::size_t tail_count = sink.samples.end() - it;
        std::printf("run %zu: %zu samples >= 5us\n", i, tail_count);
        if (i == 4) {
            struct W {const char* name; uint16_t loc; };
            constexpr W kW[] = {{"AAPL", 13},{"AMZN",398},{"GOOGL",3461},
                        {"MSFT",5294},{"QQQ",6562},{"SPY",7457},{"TSLA",8000}};
            uint64_t tot_far = 0, tot_nf = 0;
            for (const auto& w : kW) {
                hft::OrderBook* b = books.get(w.loc);
                if (!b) continue;
                std::printf("%-6s far=%llu not_found=%llu\n",
                            w.name,
                            (unsigned long long)b->far_orders(),
                            (unsigned long long)b->not_found());
                tot_far += b->far_orders();
                tot_nf  += b->not_found();
            }
            std::printf("TOTAL far=%llu not_found=%llu\n",
                        (unsigned long long)tot_far, (unsigned long long)tot_nf);
        }
        uint64_t sz = sink.samples.size();
        if (sz == 0) {
            std::fprintf(stderr, "run %zu produced no samples\n", i);
            return 1;
        }
        std::printf("callibrated TSC: %.3f GHz\n", freq / 1e9);
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
