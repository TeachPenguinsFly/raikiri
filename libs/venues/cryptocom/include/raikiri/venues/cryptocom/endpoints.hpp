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

// Per method, per IP; config/venues.yaml holds half of it.
inline constexpr std::uint32_t k_public_rest_per_method_per_s = 100;

// A larger subscribe frame is dropped whole, with no error and no data.
inline constexpr std::size_t k_max_frame_bytes = 4000;

} // namespace raikiri::venues::cryptocom::ep
