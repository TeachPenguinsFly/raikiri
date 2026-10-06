#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::pacifica {

using WireError = wire::Error;

// one row of GET /api/v1/info
struct WireMarket {
    std::string_view symbol; // bare base: "BTC", "kBONK"; spot rows are "SOL-USDC"
    std::string_view type;   // "perpetual" or "spot"
    std::string_view tick;
    std::string_view lot;
    std::string_view min_notional; // min_order_size, in USD
    bool rfq{false};
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

enum class Channel : std::uint8_t { Unknown = 0, Book, Bbo, Trades, Prices, Pong, Subscribed, Error };

[[nodiscard]] Channel parse_channel(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every book frame is the whole ladder; `li` is the engine's event id across symbols
struct WireBook {
    std::string_view symbol;
    std::uint64_t li{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireBbo {
    std::string_view symbol;
    std::uint64_t li{0};
    std::uint64_t ts_ms{0};
    std::string_view bid;
    std::string_view bid_qty;
    std::string_view ask;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct WirePrint {
    std::string_view symbol;
    std::uint64_t history_id{0};
    std::uint64_t ts_ms{0};
    std::string_view price;
    std::string_view qty;
    std::string_view direction; // open_long, open_short, close_long, close_short
};

inline constexpr std::size_t k_max_prints = 512;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

// a row of the `prices` stream and of GET /api/v1/info/prices
struct WirePrice {
    std::string_view symbol;
    std::string_view funding;      // fixed for the next settlement
    std::string_view next_funding; // running estimate for the one after
    std::string_view mark;
    std::string_view oracle;        // the index
    std::string_view volume_24h;    // USD
    std::string_view open_interest; // base units
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_prices = 1024;

struct PriceBuffer {
    std::array<WirePrice, k_max_prices> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePrice>, WireError> parse_prices(std::string_view frame,
                                                                                PriceBuffer& storage);

} // namespace raikiri::venues::pacifica
