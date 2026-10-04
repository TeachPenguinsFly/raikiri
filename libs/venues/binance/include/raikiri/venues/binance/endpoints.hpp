#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <string_view>

namespace raikiri::venues::binance::ep {

inline constexpr std::string_view k_rest_live = "fapi.binance.com";
inline constexpr std::string_view k_rest_test = "testnet.binancefuture.com";
inline constexpr std::string_view k_stream_live = "fstream.binance.com";
inline constexpr std::string_view k_stream_test = "fstream.binancefuture.com";

inline constexpr std::string_view k_spot_rest_live = "api.binance.com";
inline constexpr std::string_view k_spot_rest_test = "testnet.binance.vision";
inline constexpr std::string_view k_spot_stream_live = "stream.binance.com";
inline constexpr std::string_view k_spot_stream_test = "stream.testnet.binance.vision";

// options stream from the futures host and share its testnet
inline constexpr std::string_view k_option_rest_live = "eapi.binance.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet, MarketType market = MarketType::Perp) noexcept {
    if (market == MarketType::Spot) {
        return testnet ? k_spot_rest_test : k_spot_rest_live;
    }
    if (market == MarketType::Option && !testnet) {
        return k_option_rest_live;
    }
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet, MarketType market = MarketType::Perp) noexcept {
    if (market == MarketType::Spot) {
        return testnet ? k_spot_stream_test : k_spot_stream_live;
    }
    return testnet ? k_stream_test : k_stream_live;
}

inline constexpr std::string_view k_ws_api_live = "ws-fapi.binance.com";
inline constexpr std::string_view k_ws_api_test = "testnet.binancefuture.com";
inline constexpr std::string_view k_ws_api_path = "/ws-fapi/v1";

[[nodiscard]] constexpr std::string_view ws_api_host(bool testnet) noexcept {
    return testnet ? k_ws_api_test : k_ws_api_live;
}

inline constexpr std::string_view k_ws_combined = "/stream";
// markPrice, aggTrade and !markPrice@arr deliver only here; bookTicker, depth and trade only on /stream
inline constexpr std::string_view k_ws_market = "/market/stream";
// options split the other way: bookTicker, depth and optionTrade on /public, optionMarkPrice on /market
inline constexpr std::string_view k_ws_public = "/public/stream";
// the listen key is appended at the call site; the legacy /ws/ path connects but delivers no account events
inline constexpr std::string_view k_ws_user_prefix = "/private/ws/";

// post places, delete cancels, put amends, get reads
inline constexpr std::string_view k_order = "/fapi/v1/order";

inline constexpr std::string_view k_open_orders = "/fapi/v1/openOrders";
inline constexpr std::string_view k_income = "/fapi/v1/income";
inline constexpr std::string_view k_user_trades = "/fapi/v1/userTrades";
inline constexpr std::string_view k_position_risk = "/fapi/v3/positionRisk";
// the only read that states multiAssetsMargin beside the per-asset rows; v3 dropped it
inline constexpr std::string_view k_account = "/fapi/v2/account";
// post opens, put keeps alive, delete closes
inline constexpr std::string_view k_listen_key = "/fapi/v1/listenKey";

inline constexpr std::string_view k_exchange_info = "/fapi/v1/exchangeInfo";
inline constexpr std::string_view k_premium_index = "/fapi/v1/premiumIndex";
// every symbol in one call: weight 40 against 1 per symbol
inline constexpr std::string_view k_ticker_24h = "/fapi/v1/ticker/24hr";
inline constexpr std::string_view k_depth = "/fapi/v1/depth";
inline constexpr std::string_view k_server_time = "/fapi/v1/time";
// the only place binance states the settlement interval
inline constexpr std::string_view k_funding_info = "/fapi/v1/fundingInfo";

// unfiltered, the listing is 6.7 MB with every halted pair and a permission matrix per row
inline constexpr std::string_view
    k_spot_exchange_info = "/api/v3/exchangeInfo?symbolStatus=TRADING&showPermissionSets=false";
inline constexpr std::string_view k_spot_ticker_24h = "/api/v3/ticker/24hr";
inline constexpr std::string_view k_spot_depth = "/api/v3/depth";
inline constexpr std::string_view k_spot_server_time = "/api/v3/time";

inline constexpr std::string_view k_option_exchange_info = "/eapi/v1/exchangeInfo";
inline constexpr std::string_view k_option_server_time = "/eapi/v1/time";

} // namespace raikiri::venues::binance::ep
