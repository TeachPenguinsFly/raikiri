#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bullish {

using WireError = wire::Error;

// one row of GET /trading-api/v1/markets
struct WireMarket {
    std::string_view symbol; // "BTCUSDC", "BTC-USDC-PERP"
    std::string_view base;
    std::string_view quote;
    std::string_view type; // SPOT, PERPETUAL, DATED_FUTURE, OPTION
    std::string_view tick;
    std::string_view min_qty;
    std::string_view multiplier;
    std::uint32_t price_precision{0};
    std::uint32_t quantity_precision{0};
    bool enabled{false};
};

inline constexpr std::size_t k_max_markets = 4096;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

// one row of GET /trading-api/v1/index-prices, keyed by the asset alone
struct WireIndex {
    std::string_view asset;
    std::string_view price;
};

inline constexpr std::size_t k_max_index_rows = 512;

struct IndexBuffer {
    std::array<WireIndex, k_max_index_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireIndex>, WireError> parse_index_prices(std::string_view body,
                                                                                      IndexBuffer& storage);

enum class Kind : std::uint8_t { Unknown = 0, Snapshot, Update, Reply, Refusal };

enum class DataType : std::uint8_t { Unknown = 0, Book, Tick, Trades };

struct WireHead {
    Kind kind{Kind::Unknown};
    DataType data{DataType::Unknown};
};

[[nodiscard]] std::expected<WireHead, WireError> parse_head(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every frame is the whole top of the book; the ranges of consecutive frames meet
struct WireBook {
    std::string_view symbol;
    std::uint64_t seq_first{0};
    std::uint64_t seq_last{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireTick {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::string_view quote_volume; // 24h
    std::string_view mark;         // derivatives only
    std::string_view funding_rate; // perpetuals only, in percent
    std::string_view open_interest;
};

[[nodiscard]] std::expected<WireTick, WireError> parse_tick(std::string_view frame);

struct WirePrint {
    std::string_view symbol;
    std::uint64_t trade_id{0};
    std::string_view price;
    std::string_view qty;
    std::string_view side;
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_prints = 256;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

} // namespace raikiri::venues::bullish
