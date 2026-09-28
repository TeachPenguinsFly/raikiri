#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitunix {

using WireError = wire::Error;

inline constexpr std::int64_t k_ok = 0;
inline constexpr std::int64_t k_rate_limited = 100009;

// the futures host states `code` as a number, the spot host as a string
struct WireStatus {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<WireStatus, WireError> parse_status(std::string_view body);

struct WirePair {
    std::string_view symbol;
    std::string_view base;
    std::string_view quote;
    std::string_view min_qty;
    std::string_view status;
    std::string_view max_funding;
    std::string_view min_funding;
    std::uint64_t base_precision{0};
    std::uint64_t quote_precision{0};
    bool api{false};
};

inline constexpr std::size_t k_max_pairs = 2048;

struct PairBuffer {
    std::array<WirePair, k_max_pairs> rows{};
};

[[nodiscard]] std::expected<std::span<const WirePair>, WireError> parse_pairs(std::string_view body,
                                                                              PairBuffer& storage);

[[nodiscard]] std::expected<std::span<const WirePair>, WireError> parse_spot_pairs(std::string_view body,
                                                                                   PairBuffer& storage);

// rates on this endpoint are in percent
struct WireFunding {
    std::string_view symbol;
    std::string_view mark;
    std::string_view rate;
    std::uint64_t interval_h{0};
    std::uint64_t next_ms{0};
};

inline constexpr std::size_t k_max_funding = 2048;

struct FundingBuffer {
    std::array<WireFunding, k_max_funding> rows{};
};

[[nodiscard]] std::expected<std::span<const WireFunding>, WireError> parse_funding(std::string_view body,
                                                                                   FundingBuffer& storage);

struct WireTicker {
    std::string_view symbol;
    std::string_view base_volume;
    std::string_view quote_volume;
};

struct TickerBuffer {
    std::array<WireTicker, k_max_funding> rows{};
};

[[nodiscard]] std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body,
                                                                                  TickerBuffer& storage);

enum class Channel : std::uint8_t { Unknown = 0, Book, Trade, Pong, Connect, Refused };

struct WireHead {
    Channel channel{Channel::Unknown};
    std::string_view ch;
    std::string_view symbol;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] WireHead parse_head(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_levels = 256;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// every depth frame is the whole ladder to its depth; there is no sequence
struct WireBook {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

// the spot ladder is aggregated at the requested precision and stamped to the second only
[[nodiscard]] std::expected<WireBook, WireError> parse_spot_depth(std::string_view body, LevelBuffer& storage);

// a print carries no id and a whole-second time; the envelope's ts is the millisecond stamp
struct WirePrint {
    std::string_view price;
    std::string_view qty;
    std::string_view side;
};

inline constexpr std::size_t k_max_prints = 512;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

struct WireTrades {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WirePrint> prints;
};

[[nodiscard]] std::expected<WireTrades, WireError> parse_trades(std::string_view frame, PrintBuffer& storage);

} // namespace raikiri::venues::bitunix
