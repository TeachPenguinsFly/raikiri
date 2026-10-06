#pragma once

#include <string_view>

namespace raikiri::venues::paradex::ep {

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? "api.testnet.paradex.trade" : "api.prod.paradex.trade";
}

[[nodiscard]] constexpr std::string_view ws_host(bool testnet) noexcept {
    return testnet ? "ws.api.testnet.paradex.trade" : "ws.api.prod.paradex.trade";
}

inline constexpr std::string_view k_markets = "/v1/markets";
inline constexpr std::string_view k_time = "/v1/system/time";

// the public socket refuses json data since 2026-09-21; 1:1 is the newest version it negotiates
inline constexpr std::string_view k_ws_path = "/v1?sbeSchemaId=1&sbeSchemaVersion=1";

} // namespace raikiri::venues::paradex::ep
