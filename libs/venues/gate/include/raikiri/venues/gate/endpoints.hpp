#pragma once

// futures paths are k_futures_prefix + settle + tail

#include <string_view>

namespace raikiri::venues::gate::ep {

inline constexpr std::string_view k_rest_live = "api.gateio.ws";
inline constexpr std::string_view k_rest_test = "api-testnet.gateapi.io";
inline constexpr std::string_view k_futures_stream_live = "fx-ws.gateio.ws";
inline constexpr std::string_view k_futures_stream_test = "fx-ws-testnet.gateio.ws";
// spot streams from the rest host; its testnet has another name
inline constexpr std::string_view k_spot_stream_live = "api.gateio.ws";
inline constexpr std::string_view k_spot_stream_test = "ws-testnet.gate.com";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view futures_stream_host(bool testnet) noexcept {
    return testnet ? k_futures_stream_test : k_futures_stream_live;
}

inline constexpr std::string_view k_option_stream_live = "op-ws.gateio.live";
inline constexpr std::string_view k_option_stream_test = "ws-testnet.gate.com";

[[nodiscard]] constexpr std::string_view option_stream_host(bool testnet) noexcept {
    return testnet ? k_option_stream_test : k_option_stream_live;
}

[[nodiscard]] constexpr std::string_view spot_stream_host(bool testnet) noexcept {
    return testnet ? k_spot_stream_test : k_spot_stream_live;
}

inline constexpr std::string_view k_ws_futures_usdt = "/v4/ws/usdt";
// the live spot socket keeps its trailing slash; testnet uses another path
inline constexpr std::string_view k_ws_spot_live = "/ws/v4/";
inline constexpr std::string_view k_ws_spot_test = "/v4/ws/spot";

[[nodiscard]] constexpr std::string_view ws_spot_path(bool testnet) noexcept {
    return testnet ? k_ws_spot_test : k_ws_spot_live;
}

inline constexpr std::string_view k_ws_option_live = "/v4/ws";
inline constexpr std::string_view k_ws_option_test = "/v4/ws/options";

[[nodiscard]] constexpr std::string_view ws_option_path(bool testnet) noexcept {
    return testnet ? k_ws_option_test : k_ws_option_live;
}

// the settlement currency goes here, then a tail below
inline constexpr std::string_view k_futures_prefix = "/api/v4/futures/";

// one tail for four verbs; an order id is appended when there is one
inline constexpr std::string_view k_futures_orders = "/orders";
inline constexpr std::string_view k_futures_batch_cancel = "/batch_cancel_orders";
inline constexpr std::string_view k_spot_orders = "/api/v4/spot/orders";
inline constexpr std::string_view k_spot_cancel_batch = "/api/v4/spot/cancel_batch_orders";

// plain /my_trades pages by order id; only the timerange variant takes from/to
inline constexpr std::string_view k_futures_my_trades = "/my_trades_timerange";
inline constexpr std::string_view k_futures_positions = "/positions";
inline constexpr std::string_view k_futures_account_book = "/account_book";
inline constexpr std::string_view k_futures_accounts = "/accounts";
// one entry per currency pair, each with its own orders array
inline constexpr std::string_view k_spot_open_orders = "/api/v4/spot/open_orders";
inline constexpr std::string_view k_spot_my_trades = "/api/v4/spot/my_trades";

inline constexpr std::string_view k_futures_contracts = "/contracts";
inline constexpr std::string_view k_futures_tickers = "/tickers";
inline constexpr std::string_view k_futures_order_book = "/order_book";
inline constexpr std::string_view k_spot_currency_pairs = "/api/v4/spot/currency_pairs";
inline constexpr std::string_view k_server_time = "/api/v4/spot/time";

inline constexpr std::string_view k_option_underlyings = "/api/v4/options/underlyings";
// one underlying per call: ?underlying=BTC_USDT
inline constexpr std::string_view k_option_contracts = "/api/v4/options/contracts";

} // namespace raikiri::venues::gate::ep
