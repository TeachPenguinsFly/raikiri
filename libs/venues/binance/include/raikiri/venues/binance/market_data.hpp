#pragma once

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// the same shape arrives on @trade and @aggTrade; the account's own fill is WireTrade
struct WirePrint {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0}; // aggregate id or trade id, whichever arrived
    std::uint64_t trade_ms{0};
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view data);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data);

// GET /fapi/v1/premiumIndex: lastFundingRate for r, nextFundingTime for T
inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WireMarkPrice, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarkPrice>, WireError> parse_premium_index(std::string_view body,
                                                                                           PremiumBuffer& storage);

struct WireTicker24h {
    std::string_view symbol;
    std::string_view volume;       // base asset
    std::string_view quote_volume; // quote currency
    std::uint64_t close_ms{0};
};

inline constexpr std::size_t k_max_ticker_rows = 4096;

struct Ticker24hBuffer {
    std::array<WireTicker24h, k_max_ticker_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker24h>, WireError> parse_ticker_24h(std::string_view body,
                                                                                        Ticker24hBuffer& storage);

} // namespace raikiri::venues::binance
