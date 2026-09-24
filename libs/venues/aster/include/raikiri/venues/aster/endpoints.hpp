#pragma once

// fapi market data and funding only; order entry needs an evm wallet signer that is not built

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::aster::ep {

// aster has no testnet host, so the testnet flag is ignored
inline constexpr std::string_view k_rest_live = "fapi.asterdex.com";
inline constexpr std::string_view k_stream_live = "fstream.asterdex.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    (void)testnet;
    return k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    (void)testnet;
    return k_stream_live;
}

inline constexpr std::string_view k_ws_combined = "/stream";

inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";
// exchangeInfo does not carry the funding interval
inline constexpr std::string_view k_funding_info = "/fapi/v1/fundingInfo";

} // namespace raikiri::venues::aster::ep
