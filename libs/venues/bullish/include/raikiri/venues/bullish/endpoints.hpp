#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::bullish::ep {

[[nodiscard]] constexpr std::string_view host(bool testnet) noexcept {
    return testnet ? "api.simnext.bullish-test.com" : "api.exchange.bullish.com";
}

inline constexpr std::string_view k_markets = "/trading-api/v1/markets";
inline constexpr std::string_view k_time = "/trading-api/v1/time";
inline constexpr std::string_view k_index_prices = "/trading-api/v1/index-prices";
inline constexpr std::uint64_t k_index_interval_ns = 5'000'000'000ULL;

// each family has its own socket and refuses the others' topics
inline constexpr std::string_view k_ws_orderbook = "/trading-api/v1/market-data/orderbook";
inline constexpr std::string_view k_ws_tick = "/trading-api/v1/market-data/tick";
inline constexpr std::string_view k_ws_trades = "/trading-api/v1/market-data/trades";

inline constexpr std::uint64_t k_keepalive_interval_ns = 30'000'000'000ULL;

inline constexpr std::string_view
    k_keepalive_frame = R"({"jsonrpc":"2.0","type":"command","method":"keepalivePing","params":{},"id":"0"})";

} // namespace raikiri::venues::bullish::ep
