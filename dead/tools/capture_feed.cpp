// capture_feed: connects to Coinbase's public market data WebSocket feed and
// writes raw JSON messages to a file, one per line, for use as an offline
// test fixture. Standalone tool — not linked into hft_core.
//
// Synchronous Boost.Beast/Asio client (blocking calls), by design: no
// io_context/async executor model yet, just connect -> handshake -> read loop.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
using json = nlohmann::json;

void auth() {
    std::ifstream key_file("/Users/ethansung/quant/HFT/cdp_api_key.json");
    json key_json;
    key_file >> key_json;
    std::string key_name = key_json["name"];
    std::string private_key_pem = key_json["privateKey"];
    std::cout << key_name << "\n";
}

int main() {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tlsv12_client};
    ctx.set_default_verify_paths();

    boost::asio::ip::tcp::resolver resolver{ioc};
    auto const results = resolver.resolve("advanced-trade-ws.coinbase.com", "443");

    boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> ws{ioc, ctx};
    ws.next_layer().set_verify_mode(boost::asio::ssl::verify_peer);
    boost::asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());
    
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "advanced-trade-ws.coinbase.com")) {
        throw boost::system::system_error(
            static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
    }
    ws.next_layer().handshake(boost::asio::ssl::stream_base::client);
    ws.handshake("advanced-trade-ws.coinbase.com", "/");

    
    std::string const subscribe_msg = R"({"type": "subscribe", "product_ids": ["BTC-USD"], "channel": "level2"})";
    ws.write(boost::asio::buffer(subscribe_msg));

    std::ofstream out("/Users/ethansung/quant/HFT-v2/core/tests/fixtures/btc_usd_l2.jsonl");

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    int count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::beast::flat_buffer buffer;
        ws.read(buffer);
        out << boost::beast::make_printable(buffer.data()) << "\n";
        ++count;
    }

    std::cout << count << "\n";

    ws.close(boost::beast::websocket::close_code::normal);

    return 0;
}
