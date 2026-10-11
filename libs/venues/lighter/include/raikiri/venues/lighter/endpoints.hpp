#pragma once

#include <cstdint>
#include <string_view>

namespace raikiri::venues::lighter::ep {

inline constexpr std::string_view k_rest_live = "mainnet.zklighter.elliot.ai";
inline constexpr std::string_view k_rest_test = "testnet.zklighter.elliot.ai";

[[nodiscard]] constexpr std::string_view rest_host(bool testnet) noexcept {
    return testnet ? k_rest_test : k_rest_live;
}

[[nodiscard]] constexpr std::string_view stream_host(bool testnet) noexcept {
    return rest_host(testnet);
}

inline constexpr std::string_view k_ws_stream = "/stream";

// Perp and spot in one body, with the funding clamps /api/v1/orderBooks lacks.
inline constexpr std::string_view k_order_book_details = "/api/v1/orderBookDetails";

// Not read: a comparison table restating every rate per 8 h, other venues' rates
// under lighter market ids.
inline constexpr std::string_view k_funding_comparison = "/api/v1/funding-rates";

inline constexpr std::string_view k_send_tx = "/api/v1/sendTx";
inline constexpr std::string_view k_next_nonce = "/api/v1/nextNonce";
inline constexpr std::string_view k_account = "/api/v1/account";
inline constexpr std::string_view k_active_orders = "/api/v1/accountActiveOrders";
inline constexpr std::string_view k_account_orders = "/api/v1/accountOrders";
inline constexpr std::string_view k_trades = "/api/v1/trades";

// the budget target of a tx sent on the socket; the venue counts it with rest sendTx
inline constexpr std::string_view k_ws_send_tx = "ws:sendtx";

// A REST 429 throttles the same IP's sockets too, so REST fetches only the catalogue.
inline constexpr std::uint32_t k_rest_requests_per_minute = 60;
inline constexpr std::uint64_t k_rest_cooldown_ns = 60ULL * 1'000'000'000ULL;

// Past the IP's socket message limit the venue answers 30009.
inline constexpr std::int64_t k_ws_rate_limited = 30009;
inline constexpr std::uint64_t k_ws_refusal_hold_ns = 10ULL * 1'000'000'000ULL;

} // namespace raikiri::venues::lighter::ep
