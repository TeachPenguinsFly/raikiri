#pragma once

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// documented as ten fields but eleven arrive; the arity is what is read
inline constexpr std::size_t k_ticker_arity = 10;
inline constexpr std::size_t k_ticker_bid = 0;
inline constexpr std::size_t k_ticker_bid_size = 1;
inline constexpr std::size_t k_ticker_ask = 2;
inline constexpr std::size_t k_ticker_ask_size = 3;
inline constexpr std::size_t k_ticker_last_price = 6;
inline constexpr std::size_t k_ticker_volume = 7;

// no timestamp in the payload
struct WireTicker {
    std::string_view bid;
    std::string_view bid_size;
    std::string_view ask;
    std::string_view ask_size;
    std::string_view last_price;
    // 24-hour volume in the base asset; the channel has no quote column
    std::string_view volume;
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// amount's sign is the aggressor: positive means the taker bought
inline constexpr std::size_t k_trade_arity = 4;
inline constexpr std::size_t k_trade_id = 0;
inline constexpr std::size_t k_trade_mts = 1;
inline constexpr std::size_t k_trade_amount = 2;
inline constexpr std::size_t k_trade_price = 3;

struct WireTrade {
    std::string_view id;
    std::string_view amount; // signed; see above
    std::string_view price;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// te and tu are the same print; publish only one
enum class TradeFrameKind : std::uint8_t {
    Snapshot, // the array-of-arrays that follows a subscribe: history
    Executed, // "te"
    Updated,  // "tu"
};

struct WireTrades {
    TradeFrameKind kind{TradeFrameKind::Executed};
    std::span<const WireTrade> trades;
};

[[nodiscard]] std::expected<WireTrades, WireError> parse_trades(std::string_view frame, TradeBuffer& storage);

// mark price (14) tracks the index and drives liquidations; deriv price (2) is the perp's own mid
inline constexpr std::size_t k_deriv_arity = 23;
inline constexpr std::size_t k_deriv_mts = 0;
inline constexpr std::size_t k_deriv_price = 2;
inline constexpr std::size_t k_deriv_spot_price = 3;
inline constexpr std::size_t k_deriv_next_funding_mts = 7;
inline constexpr std::size_t k_deriv_next_funding_accrued = 8;
inline constexpr std::size_t k_deriv_next_funding_step = 9;
inline constexpr std::size_t k_deriv_current_funding = 11;
inline constexpr std::size_t k_deriv_mark_price = 14;
inline constexpr std::size_t k_deriv_open_interest = 17;

struct WireDerivStatus {
    std::string_view deriv_price;
    std::string_view spot_price;
    std::string_view mark_price;
    std::string_view current_funding;      // rate being applied now
    std::string_view next_funding_accrued; // running estimate for the next event
    std::string_view open_interest;
    std::uint64_t ts_ms{0};
    std::uint64_t next_funding_ms{0};
};

[[nodiscard]] std::expected<WireDerivStatus, WireError> parse_deriv_status(std::string_view frame);

} // namespace raikiri::venues::bitfinex
