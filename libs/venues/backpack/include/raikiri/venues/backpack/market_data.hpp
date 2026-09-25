#pragma once

#include <raikiri/venues/backpack/envelope.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

struct WirePrint {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0};
    std::uint64_t engine_us{0};
    bool buyer_is_maker{false}; // m: true => the aggressor sold
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view data);

// an empty rate on a spot pair means no funding
struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::string_view funding_rate;
    std::uint64_t next_funding_ms{0};
    std::uint64_t event_us{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data);

// polled from GET /api/v1/tickers: one call covers every market
struct WireTicker {
    std::string_view symbol;
    std::string_view volume;       // base asset
    std::string_view quote_volume; // USDC
};

inline constexpr std::size_t k_max_tickers = 512;

struct TickerBuffer {
    std::array<WireTicker, k_max_tickers> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

} // namespace raikiri::venues::backpack
