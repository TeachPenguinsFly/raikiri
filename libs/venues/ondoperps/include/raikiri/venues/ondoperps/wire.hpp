#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::ondoperps {

using WireError = wire::Error;

// one perp of GET /v1/markets
struct WireMarket {
    std::string_view market; // "BTC-USD.P"
    std::string_view base;   // "BTC", "US500", "USDJPY"
    std::string_view quote;  // "USD"
    std::string_view tick;
    std::string_view lot;
    std::string_view maker_fee;
    std::string_view taker_fee;
    std::string_view funding_cap; // per hour
    std::string_view tag;         // Crypto, Stock, ETF, Commodity, Index, FX
    bool disabled{false};
};

inline constexpr std::size_t k_max_markets = 512;

struct MarketBuffer {
    std::array<WireMarket, k_max_markets> markets{};
};

[[nodiscard]] std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                                  MarketBuffer& storage);

// iso-8601 utc with up to nine fractional digits, as unix nanoseconds
[[nodiscard]] std::optional<std::uint64_t> iso_to_ns(std::string_view text) noexcept;

// the clock probe: the depth answer's `time`
[[nodiscard]] std::optional<std::uint64_t> server_time(std::string_view body) noexcept;

enum class Frame : std::uint8_t { Unknown = 0, Top, Depth, Trades, Marks, Funding, Subscribed, Pong, Error };

[[nodiscard]] Frame parse_frame(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 256;

// a book row; a top-of-book row has at most one level a side
struct WireBook {
    std::string_view market;
    std::uint64_t ts_ns{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

inline constexpr std::size_t k_max_rows = 512;

struct BookBuffer {
    std::array<WireBook, k_max_rows> rows{};
    std::array<WireLevel, k_max_levels * 2> levels{};
};

// the rows of a topOfBooksPerps or depthBooksPerps update; levels share one pool
[[nodiscard]] std::expected<std::span<const WireBook>, WireError> parse_books(std::string_view frame,
                                                                              BookBuffer& storage);

struct WirePrint {
    std::string_view market;
    std::string_view price;
    std::string_view size;
    std::string_view side; // the aggressor's
    std::uint64_t ts_ns{0};
};

struct PrintBuffer {
    std::array<WirePrint, k_max_rows> prints{};
};

[[nodiscard]] std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                                PrintBuffer& storage);

struct WireMark {
    std::string_view market;
    std::string_view mark;
    std::string_view oracle;
    std::uint64_t ts_ns{0};
};

struct MarkBuffer {
    std::array<WireMark, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireMark>, WireError> parse_marks(std::string_view frame,
                                                                              MarkBuffer& storage);

// the running rate of the hour that ends at `ends_ns`
struct WireFunding {
    std::string_view market;
    std::string_view rate;
    std::uint64_t ends_ns{0};
    std::uint64_t ts_ns{0}; // the envelope's
};

struct FundingBuffer {
    std::array<WireFunding, k_max_rows> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFunding>, WireError> parse_funding(std::string_view frame,
                                                                                   FundingBuffer& storage);

// a row of /v1/perps/open_interest (openInterest) or /v1/perps/volume (volume, quoteVolume)
struct WireStat {
    std::string_view market;
    std::string_view first;
    std::string_view second;
};

struct StatBuffer {
    std::array<WireStat, k_max_markets> rows{};
};

[[nodiscard]] std::expected<std::span<const WireStat>, WireError> parse_open_interest(std::string_view body,
                                                                                      StatBuffer& storage);

[[nodiscard]] std::expected<std::span<const WireStat>, WireError> parse_volumes(std::string_view body,
                                                                                StatBuffer& storage);

} // namespace raikiri::venues::ondoperps
