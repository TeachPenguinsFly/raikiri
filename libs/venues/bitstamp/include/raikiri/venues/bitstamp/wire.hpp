#pragma once

// numbers arrive as decimal strings, stamps in seconds beside a microsecond string

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitstamp {

using WireError = wire::Error;

inline constexpr std::size_t k_max_markets = 1024;

struct WireMarket {
    std::string_view market_symbol; // btcusd, btcusd-perp
    std::string_view base_currency;
    std::string_view counter_currency;
    std::string_view market_type; // SPOT, PERPETUAL
    std::string_view asset_class; // perpetuals only: CRYPTO, COMMODITIES, ETF, FX
    std::string_view trading;     // Enabled
    std::string_view minimum_order_value;
    std::string_view contract_size;
    std::uint32_t base_decimals{0};
    std::uint32_t counter_decimals{0};
};

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

// the pair is "BTC/USD" or "BTC/USD-PERP"; volume and vwap cover the last 24 hours
struct WireTicker {
    std::string_view pair;
    std::string_view volume;
    std::string_view vwap;
    std::string_view open_interest; // perpetuals only, in the base
    std::uint64_t ts_s{0};
};

struct TickerBuffer {
    std::array<WireTicker, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

enum class Event : std::uint8_t { Unknown = 0, Book, Trade, Funding, Subscribed, Refused, Reconnect };

struct Head {
    Event event{Event::Unknown};
    std::string_view channel{};
};

[[nodiscard]] Head parse_head(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 100;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// order_book_ frames carry the whole top hundred levels each time
struct WireBook {
    std::span<const WireLevel> bids{};
    std::span<const WireLevel> asks{};
    std::uint64_t ts_us{0};
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

// type 0 is a buy by the aggressor, 1 a sell
struct WireTrade {
    std::uint64_t id{0};
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::uint64_t ts_us{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

struct WireFunding {
    std::string_view market;
    std::string_view rate;
    std::string_view mark;
    std::string_view index;
    std::uint64_t ts_s{0};
    std::uint64_t next_s{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

} // namespace raikiri::venues::bitstamp
