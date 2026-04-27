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

// The live host refuses any path; the testnet one wants /ws.
[[nodiscard]] constexpr std::string_view stream_path(bool testnet) noexcept {
    return testnet ? "/ws" : "/";
}

// Served from a CDN cache up to ~25 minutes old, whatever the query string.
inline constexpr std::string_view k_products = "/public/products";

// /public/* is a fixed window of 300 requests a minute per IP, answered past it
// with 429 and x-ratelimit-retry-after, not a ban. /md/* is a separate bucket
// that states no headers. This adapter asks /public/products only.
inline constexpr std::uint32_t k_public_requests_per_window = 300;
inline constexpr std::uint64_t k_public_window_ns = 60ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::phemex::ep
