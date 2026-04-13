#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::cryptocom::ep {

inline constexpr std::string_view k_rest_live = "api.crypto.com";
inline constexpr std::string_view k_rest_uat = "uat-api.3ona.co";
inline constexpr std::string_view k_stream_live = "stream.crypto.com";
inline constexpr std::string_view k_stream_uat = "uat-stream.3ona.co";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_uat : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_uat : k_stream_live;
}

inline constexpr std::string_view k_ws_market = "/exchange/v1/market";
inline constexpr std::string_view k_instruments = "/exchange/v1/public/get-instruments";

// Per method, per IP. No metering header, so config/venues.yaml has no budget.
inline constexpr std::uint32_t k_public_rest_per_method_per_s = 100;
inline constexpr std::uint32_t k_ws_requests_per_s = 100;

// Past this the venue refuses each subscribe with 40107.
inline constexpr std::size_t k_max_channels_per_connection = 400;

// A larger subscribe frame is dropped whole, with no error and no data.
inline constexpr std::size_t k_max_frame_bytes = 4000;

} // namespace raikiri::venues::cryptocom::ep
