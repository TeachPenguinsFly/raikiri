#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace raikiri::venues::paradex {

using WireError = wire::Error;

// one perpetual or spot row of GET /v1/markets; the options in the same body are skipped
struct WireMarket {
    std::string_view symbol; // "BTC-USD-PERP", "ETH-USD"
    std::string_view base;
    std::string_view quote;
    std::string_view kind; // "PERP" or "SPOT"
    std::string_view tick;
    std::string_view lot;
    std::string_view min_notional;
    std::string_view max_funding_rate;
    std::uint32_t funding_period_hours{0};
    bool rwa{false};
};

inline constexpr std::size_t k_max_markets = 512;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
    std::size_t listed{0};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

// the json-rpc answer to a subscribe or unsubscribe; the data itself is sbe
struct WireReply {
    std::uint64_t id{0};
    std::int64_t code{0}; // zero on success
    std::string_view channel;
};

[[nodiscard]] std::expected<WireReply, WireError> parse_reply(std::string_view frame);

enum class Template : std::uint16_t {
    Unknown = 0,
    Trade = 1,
    Bbo = 2,
    Book = 3,
    Summary = 4,
    Funding = 5,
};

// true for an sbe frame of schema 1; a json text frame never reads as one
[[nodiscard]] bool is_sbe(std::string_view frame) noexcept;

[[nodiscard]] Template template_of(std::string_view frame) noexcept;

inline constexpr std::int64_t k_null = std::numeric_limits<std::int64_t>::min();

// prices, sizes and rates are int64 mantissas over 1e8; timestamps are microseconds
struct WireLevel {
    std::int64_t price{0};
    std::int64_t qty{0};
};

inline constexpr std::size_t k_max_levels = 4096;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireBook {
    std::string_view market;
    std::int64_t ts_us{0};
    std::int64_t seq{0};
    bool snapshot{false};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WireBbo {
    std::string_view market;
    std::int64_t ts_us{0};
    std::int64_t seq{0};
    std::int64_t bid{k_null};
    std::int64_t bid_qty{k_null};
    std::int64_t ask{k_null};
    std::int64_t ask_qty{k_null};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct WirePrint {
    std::string_view market;
    std::int64_t ts_us{0};
    std::int64_t trade_id{0};
    std::uint8_t side{0}; // the taker's: 1 buy, 2 sell
    std::int64_t price{0};
    std::int64_t qty{0};
    std::int64_t created_us{0};
    std::uint8_t type{0}; // 1 fill, 2 liquidation, 5 rpi, 6 block trade
};

[[nodiscard]] std::expected<WirePrint, WireError> parse_trade(std::string_view frame);

struct WireSummary {
    std::string_view market;
    std::int64_t ts_us{0};
    std::int64_t mark{0};
    std::int64_t index{0};
    std::int64_t volume_usd{0};
    std::int64_t open_interest{0};
    std::int64_t funding_rate{0};
};

[[nodiscard]] std::expected<WireSummary, WireError> parse_summary(std::string_view frame);

struct WireFunding {
    std::string_view market;
    std::int64_t ts_us{0};
    std::int64_t rate{0};  // over funding_period_hours
    std::int64_t index{0}; // the cumulative funding index, not a price
    std::int64_t created_us{0};
    std::int64_t premium{0};
    std::int16_t period_hours{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

} // namespace raikiri::venues::paradex
