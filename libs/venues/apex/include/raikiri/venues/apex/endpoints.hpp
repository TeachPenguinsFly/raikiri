#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::apex::ep {

inline constexpr std::string_view k_rest_live = "omni.apex.exchange";
inline constexpr std::string_view k_rest_test = "testnet.omni.apex.exchange";
inline constexpr std::string_view k_stream_live = "quote.omni.apex.exchange";
inline constexpr std::string_view k_stream_test = "qa-quote.omni.apex.exchange";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

inline constexpr std::string_view k_ws_public = "/realtime_public";

inline constexpr std::string_view k_symbols = "/api/v3/symbols";

// per ip, all public paths share one fixed window; no header reports what is left
inline constexpr std::uint32_t k_rest_requests_per_minute = 600;

// the venue bans with 403 and the ban may be permanent
inline constexpr int k_ban_status = 403;

} // namespace raikiri::venues::apex::ep
