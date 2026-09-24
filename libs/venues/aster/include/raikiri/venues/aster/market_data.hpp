#pragma once

// funding and the 24-hour window are rest polls; markPriceUpdate is acknowledged and never sends

#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::aster {

// same shape on @trade and @aggTrade
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

// rest stand-in for markPriceUpdate, which never sends on this venue
inline constexpr std::size_t k_max_premium_rows = 2048;

struct PremiumBuffer {
    std::array<WireMarkPrice, k_max_premium_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarkPrice>, WireError>
parse_premium_index(std::string_view body, PremiumBuffer& storage);

struct WireTicker24h {
    std::string_view symbol;
    std::string_view volume;       // base asset
    std::string_view quote_volume; // quote currency
    std::uint64_t close_ms{0};
};

inline constexpr std::size_t k_max_ticker_rows = 2048;

struct Ticker24hBuffer {
    std::array<WireTicker24h, k_max_ticker_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker24h>, WireError>
parse_ticker_24h(std::string_view body, Ticker24hBuffer& storage);

} // namespace raikiri::venues::aster
