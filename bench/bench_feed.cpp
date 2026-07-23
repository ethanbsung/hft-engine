#include <fstream>
#include <cstdint>
#include <cstdio>
#include "hft/feed_handler.hpp"
#include "hft/orderbook.hpp"
#include "hft/platform.hpp"
#include <vector>
#include <cstddef>
#include "bench_util.hpp"
#include <algorithm>

int main() {
    constexpr const char* kFixturePath = "tests/fixtures/itch_orderflow.bin";
    hft::OrderBook coldBook(7457, 30392, 4096, 1 << 16);
    hft::Handler coldHandler;

    std::ifstream in(kFixturePath, std::ios::binary | std::ios::ate);
    if (!in) return 1;
    std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<std::byte> buf(n);
    in.read(reinterpret_cast<char*>(buf.data()), n);

    std::size_t frames = coldHandler.decode(buf, 0, coldBook);

    hft::nanos_t best = INT64_MAX;
    constexpr int kIters = 20;

    for (size_t i = 0; i < kIters; i++) {
        hft::OrderBook book(7457, 30392, 4096, 1 << 16);
        hft::Handler handler;
        hft::nanos_t start = hft::platform::now_ns();
        frames = handler.decode(buf, 0, book);
        
        hft::nanos_t end = hft::platform::now_ns();
        do_not_optimize(frames);
        do_not_optimize(book.best_bid());
        best = std::min(best, end - start);
    }

    double ns_per_msg = static_cast<double>(best) / frames;
    double msgs_per_sec = frames / (best / 1e9);

    std::printf("[%s] decode: %.2f ns/msg, %.0f msgs/sec over %d iters\n",
                hft::platform::platform_tag(), ns_per_msg, msgs_per_sec, kIters);

    return 0;
}
