#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::lbank::ep {

inline constexpr std::string_view k_perp_rest_host = "lbkperp.lbank.com";
inline constexpr std::string_view k_perp_ws_host = "lbkperpws.lbank.com";
inline constexpr std::string_view k_perp_ws_path = "/ws";

inline constexpr std::string_view k_perp_instruments = "/cfd/openApi/v1/pub/instrument?productGroup=SwapU";
inline constexpr std::string_view k_perp_market_data = "/cfd/openApi/v1/pub/marketData?productGroup=SwapU";

inline constexpr std::string_view k_spot_host = "api.lbank.info";
inline constexpr std::string_view k_spot_ws_path = "/ws/V2/";

inline constexpr std::string_view k_spot_accuracy = "/v2/accuracy.do";
inline constexpr std::string_view k_spot_tickers = "/v2/ticker/24hr.do?symbol=all";

inline constexpr std::string_view k_topic_trades = "2";
inline constexpr std::string_view k_topic_overview = "7";
inline constexpr std::string_view k_topic_book = "25";

inline constexpr std::string_view k_exchange = "Exchange";
inline constexpr std::string_view k_product_group = "SwapU";

inline constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

inline constexpr std::string_view k_perp_ping = "ping";

inline constexpr std::string_view k_spot_ping = R"({"action":"ping","ping":"raikiri"})";

} // namespace raikiri::venues::lbank::ep
