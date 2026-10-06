#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::vest {

using WireError = wire::Error;

// one row of GET /v2/exchangeInfo
struct WireMarket {
    std::string_view symbol; // "BTC-PERP", "NVDA-USD-PERP", "EUR-USD-PERP"
    std::string_view base;   // "BTC", "NVDA-USD", "EUR-USD"
    std::string_view quote;  // "USDC"
    std::string_view asset;  // crypto, stock, forex
    std::string_view status; // TRADING or HALT
    std::string_view taker_fee;
    std::uint32_t price_decimals{0};
    std::uint32_t size_decimals{0};
};

inline constexpr std::size_t k_max_markets = 1024;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

enum class Channel : std::uint8_t { Unknown = 0, Depth, Trades, Subscribed, Pong };

[[nodiscard]] Channel parse_channel(std::string_view frame);

// a subscribe answer's refused streams
[[nodiscard]] std::size_t count_refusals(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every depth frame is the whole book
struct WireBook {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WirePrint {
    std::string_view symbol;
    std::string_view id;
    std::string_view price;
    std::string_view qty;
    std::string_view side; // the taker's: buy or sell
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view frame);

// a row of GET /v2/ticker/latest
struct WireTicker {
    std::string_view symbol;
    std::string_view mark;
    std::string_view index;
    std::string_view funding; // oneHrFundingRate
    std::string_view status;
};

struct TickerBuffer {
    std::array<WireTicker, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

// a row of GET /v3/ticker/24hr, or of GET /v3/oi with long and short in the two fields
struct WireStat {
    std::string_view symbol;
    std::string_view first;
    std::string_view second;
};

struct StatBuffer {
    std::array<WireStat, k_max_markets> rows{};
};

// base volume, then quote volume; live and funded accounts, not evaluation ones
[[nodiscard]] std::expected<std::span<const WireStat>, WireError> parse_volumes(std::string_view body,
                                                                                StatBuffer& storage);

// long, then short open interest in the base asset
[[nodiscard]] std::expected<std::span<const WireStat>, WireError> parse_open_interest(std::string_view body,
                                                                                      StatBuffer& storage);

} // namespace raikiri::venues::vest
