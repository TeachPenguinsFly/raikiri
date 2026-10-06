#pragma once

// market data only: the venue's terms bar the owner's residence, so no order path is built

#include <cstdint>
#include <string_view>

namespace raikiri::venues::ondoperps::ep {

inline constexpr std::string_view k_host = "api.ondoperps.xyz";

inline constexpr std::string_view k_markets = "/v1/markets";
inline constexpr std::string_view k_open_interest = "/v1/perps/open_interest";
inline constexpr std::string_view k_volume = "/v1/perps/volume";
// the depth answer's `time` is the server's clock even on an empty book
inline constexpr std::string_view k_time = "/v1/perps/depth?market=BTC-USD.P&depth=1";
inline constexpr std::string_view k_ws_path = "/ws";

inline constexpr std::string_view k_top = "topOfBooksPerps";
inline constexpr std::string_view k_depth = "depthBooksPerps";
inline constexpr std::string_view k_trades = "tradesPerps";
inline constexpr std::string_view k_marks = "markPricesPerps";
inline constexpr std::string_view k_funding = "fundingRatesPerps";

inline constexpr std::uint64_t k_stats_interval_ns = 60'000'000'000ULL;
// the venue closes a socket idle for 180 s
inline constexpr std::uint64_t k_app_ping_interval_ns = 60'000'000'000ULL;

inline constexpr std::string_view k_ping_frame = R"({"op":"ping"})";

} // namespace raikiri::venues::ondoperps::ep
