#pragma once

// weex streams no funding; it is polled from /capi/v3/market/premiumIndex

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

// t is a uuid string
struct WirePrint {
    std::string_view trade_id;  // t
    std::string_view price;     // p
    std::string_view qty;       // q, base units
    std::uint64_t trade_ms{0};  // T
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

inline constexpr std::size_t k_max_prints = 64;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

// a tradeSnapshot backfill overlaps live prints, so the caller drops it and the live prints it already held
[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

[[nodiscard]] std::uint64_t newest_trade_ms(std::span<const WirePrint> prints) noexcept;

// the only place weex publishes turnover; no bid, ask or funding rate
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // v: base asset
    std::string_view quote_volume; // q: quote currency
    std::uint64_t close_ms{0};     // C: the window's close, not the send time
    std::uint64_t event_ms{0};     // E
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// one call answers for every contract; collectCycle is ignored since the catalogue carries the interval
struct WirePremium {
    std::string_view symbol;
    std::string_view mark_price;      // markPrice
    std::string_view funding_rate;    // lastFundingRate
    std::string_view forecast_rate;   // forecastFundingRate
    std::uint64_t next_funding_ms{0}; // nextFundingTime
    std::uint64_t event_ms{0};        // time
};

inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WirePremium, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePremium>, WireError> parse_premium_index(std::string_view body,
                                                                                         PremiumBuffer& storage);

} // namespace raikiri::venues::weex
