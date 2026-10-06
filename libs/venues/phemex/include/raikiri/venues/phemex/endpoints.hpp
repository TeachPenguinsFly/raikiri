#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::phemex::ep {

inline constexpr std::string_view k_rest_live = "api.phemex.com";
inline constexpr std::string_view k_rest_test = "testnet-api.phemex.com";
inline constexpr std::string_view k_stream_live = "ws.phemex.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_stream_live;
}

// the live host refuses any path; testnet wants /ws
[[nodiscard]] constexpr std::string_view stream_path(bool testnet) noexcept {
    return testnet ? "/ws" : "/";
}

// served from a cdn cache up to ~25 minutes old
inline constexpr std::string_view k_products = "/public/products";
inline constexpr std::string_view k_spot_tickers = "/md/spot/ticker/24hr/all";

// /public/* allows 300 requests a minute per ip and answers 429 past it; /md/* is separate
inline constexpr std::uint32_t k_public_requests_per_window = 300;
inline constexpr std::uint64_t k_public_window_ns = 60ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::phemex::ep
