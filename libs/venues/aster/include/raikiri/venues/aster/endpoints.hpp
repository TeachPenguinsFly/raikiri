#pragma once

// fapi market data, funding and v3 order entry signed by an evm agent wallet

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace raikiri::venues::aster::ep {

inline constexpr std::string_view k_rest_live = "fapi.asterdex.com";
inline constexpr std::string_view k_stream_live = "fstream.asterdex.com";
// the testnet's agents are made at asterdex-testnet.com/en/api-wallet, wallet only
inline constexpr std::string_view k_rest_test = "fapi.asterdex-testnet.com";
inline constexpr std::string_view k_stream_test = "fstream.asterdex-testnet.com";
// spot is its own api with the futures api's shapes
inline constexpr std::string_view k_spot_rest_live = "sapi.asterdex.com";
inline constexpr std::string_view k_spot_stream_live = "sstream.asterdex.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_stream_test : k_stream_live;
}

inline constexpr std::string_view k_ws_combined = "/stream";
inline constexpr std::string_view k_ws_user_prefix = "/ws/";

// the eip-712 domain's chainId: AsterSignTransaction signs per network
[[nodiscard]] constexpr std::uint64_t chain_id(bool testnet) noexcept {
    return testnet ? 714 : 1666;
}

inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";
// exchangeInfo does not carry the funding interval
inline constexpr std::string_view k_funding_info = "/fapi/v1/fundingInfo";

// v3 is the pro api: agent-signed, no api keys; v1 keys are no longer issued
inline constexpr std::string_view k_order = "/fapi/v3/order";
inline constexpr std::string_view k_open_orders = "/fapi/v3/openOrders";
inline constexpr std::string_view k_user_trades = "/fapi/v3/userTrades";
inline constexpr std::string_view k_income = "/fapi/v3/income";
inline constexpr std::string_view k_position_risk = "/fapi/v3/positionRisk";
inline constexpr std::string_view k_account = "/fapi/v3/account";
inline constexpr std::string_view k_listen_key = "/fapi/v3/listenKey";

inline constexpr std::string_view k_spot_exchange_info = "/api/v1/exchangeInfo";
// every symbol in one call, weight 40
inline constexpr std::string_view k_spot_ticker_24h = "/api/v1/ticker/24hr";
inline constexpr std::string_view k_spot_depth = "/api/v1/depth";
inline constexpr std::string_view k_spot_server_time = "/api/v1/time";

} // namespace raikiri::venues::aster::ep
