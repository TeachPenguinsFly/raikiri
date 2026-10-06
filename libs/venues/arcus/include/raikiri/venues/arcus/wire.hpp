#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::arcus {

using WireError = wire::Error;

// one row of GET /v1/markets
struct WireMarket {
    std::string_view symbol; // "BTC-USD", "NVDA-USD"
    std::string_view base;
    std::string_view quote;
    std::string_view type;     // "PERPETUAL"
    std::string_view status;   // "ONLINE", "OFFLINE"
    std::string_view category; // CRYPTO, EQUITIES, INDICES, COMMODITIES
    std::string_view tick;
    std::string_view step;
    std::string_view min_notional;
};

inline constexpr std::size_t k_max_markets = 512;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

enum class FrameType : std::uint8_t { Unknown = 0, Connected, Subscribed, Data, Unsubscribed, Degraded, Error };

enum class Channel : std::uint8_t { Unknown = 0, Bbo, Book, Trades, Markets };

struct WireHead {
    FrameType type{FrameType::Unknown};
    Channel channel{Channel::Unknown};
    std::string_view id;
    std::string_view message;
};

[[nodiscard]] std::expected<WireHead, WireError> parse_head(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// the subscribe snapshot or one delta; neither carries a timestamp
struct WireBook {
    std::string_view id;
    std::uint64_t seq{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireBbo {
    std::string_view id;
    std::uint64_t seq{0};
    std::uint64_t ts_us{0};
    std::string_view bid;
    std::string_view bid_qty;
    std::string_view ask;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct WirePrint {
    std::string_view symbol;
    std::string_view side; // the taker's: BUY or SELL
    std::string_view price;
    std::string_view qty;
    std::uint64_t trade_id{0};
    std::uint64_t ts_us{0};
};

inline constexpr std::size_t k_max_prints = 512;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

// a market of the `markets` channel snapshot; it carries no timestamp
struct WireStats {
    std::string_view symbol;
    std::string_view mark;
    std::string_view oracle;
    std::string_view funding;      // this hour's rate
    std::string_view next_funding; // the running estimate for the next hour
    std::uint64_t next_funding_s{0};
    std::string_view volume_notional; // USD over 24h
    std::string_view open_interest;   // base units
};

struct StatsBuffer {
    std::array<WireStats, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireStats>, WireError> parse_stats(std::string_view frame,
                                                                               StatsBuffer& storage);

} // namespace raikiri::venues::arcus
