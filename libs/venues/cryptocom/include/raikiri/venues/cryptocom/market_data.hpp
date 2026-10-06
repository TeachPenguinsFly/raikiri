#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/cryptocom/catalogue.hpp>
#include <raikiri/venues/cryptocom/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::cryptocom {

// every parser here takes Event::data, not the frame

struct WireTrade {
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    std::string_view side; // the taker's side
    std::uint64_t trade_ms{0};
};

inline constexpr std::size_t k_max_trades = 512;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view data,
                                                                                TradeBuffer& storage);

struct WireTicker {
    std::string_view instrument;    // i
    std::string_view volume;        // base
    std::string_view quote_volume;  // quote
    std::string_view open_interest; // oi, in contracts; spot rows say 0
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view data);

struct TickerBuffer {
    std::array<WireTicker, k_max_instruments> rows{};
};

// GET public/get-tickers: every instrument in one answer, rows shaped as the ticker channel's
[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

struct WireValue {
    std::string_view value;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireValue, WireError> parse_value(std::string_view data);

// rate, estimate and mark arrive on three channels and are published as one record
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    PriceTicks index{};
    std::uint64_t rate_ms{0};
};

} // namespace raikiri::venues::cryptocom
