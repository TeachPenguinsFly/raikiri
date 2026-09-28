#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::bitunix::ep {

inline constexpr std::string_view k_perp_host = "fapi.bitunix.com";
inline constexpr std::string_view k_spot_host = "openapi.bitunix.com";

inline constexpr std::string_view k_trading_pairs = "/api/v1/futures/market/trading_pairs";
inline constexpr std::string_view k_funding_batch = "/api/v1/futures/market/funding_rate/batch";
inline constexpr std::string_view k_tickers = "/api/v1/futures/market/tickers";

inline constexpr std::string_view k_spot_pairs = "/api/spot/v1/common/coin_pair/list";
inline constexpr std::string_view k_spot_depth = "/api/spot/v1/market/depth";

inline constexpr std::string_view k_ws_path = "/public/";

// a socket that sends nothing is closed however much it receives
inline constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

inline constexpr std::uint64_t k_funding_poll_ns = 60'000'000'000ULL;

// spot streams nothing public, so its books are polled one symbol at a time
inline constexpr std::uint64_t k_spot_poll_ns = 250'000'000ULL;

} // namespace raikiri::venues::bitunix::ep
