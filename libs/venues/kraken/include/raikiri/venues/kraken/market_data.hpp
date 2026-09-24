#pragma once

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// funding_rate units change with the contract family; relative_funding_rate is dimensionless
struct WireTicker {
    std::string_view product_id;
    std::string_view bid;
    std::string_view ask;
    std::string_view bid_size;
    std::string_view ask_size;
    std::string_view mark_price;
    std::string_view index;
    std::string_view last;
    std::string_view relative_funding_rate;            // dimensionless; use this
    std::string_view relative_funding_rate_prediction; // next interval forecast
    std::string_view open_interest;
    // volume counts base on PF_ and contracts on PI_; empty is not zero
    std::string_view volume;       // `volume`
    std::string_view volume_quote; // `volumeQuote`
    std::uint64_t next_funding_rate_time_ms{0};
    std::uint64_t time_ms{0};
    bool suspended{false};
};

// reads ticker and ticker_lite; lite leaves the derivative fields absent
[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// side is the aggressor's; liquidations are kept as real volume
struct WireTrade {
    std::string_view product_id;
    std::string_view uid;
    std::string_view type;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t seq{0};
    std::uint64_t time_ms{0};
};

// a trade_snapshot replays the last 100 prints
inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// reads trade and trade_snapshot through one path
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

} // namespace raikiri::venues::kraken
