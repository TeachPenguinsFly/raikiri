#pragma once

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// m is true when the buyer was the maker, so the aggressor is the opposite side
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// swap sends an array and spot a single object; both are accepted
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                                TradeBuffer& storage);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// v is base units and q quote; both optional, quoted on perp and bare on spot
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // `v`, base units
    std::string_view quote_volume; // `q`, quote units
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// the only funding source; last_funding_rate applies to the next settlement
struct WirePremiumIndex {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view last_funding_rate;
    std::uint64_t next_funding_ms{0};
    std::int64_t funding_interval_hours{0};
};

[[nodiscard]] std::expected<WirePremiumIndex, WireError> parse_premium_index(std::string_view body);

} // namespace raikiri::venues::bingx
