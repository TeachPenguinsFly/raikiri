#pragma once

#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::ostium {

using WireError = wire::Error;

// one row of GET /v1/pairs
struct WirePair {
    std::string_view id;
    std::string_view symbol; // "US500-USD", the price stream's name for it
    std::string_view from;   // "SPX"
    std::string_view to;     // "USD"
    std::string_view group;  // crypto, stocks, etf, indices, commodities, forex
};

inline constexpr std::size_t k_max_pairs = 512;

struct PairBuffer {
    std::array<WirePair, k_max_pairs> pairs{};
};

[[nodiscard]] std::expected<std::span<const WirePair>, WireError> parse_pairs(std::string_view body,
                                                                              PairBuffer& storage);

enum class Frame : std::uint8_t { Unknown = 0, Snapshot, Tick, Gap, Ack, Error };

[[nodiscard]] Frame parse_frame(std::string_view frame);

// the oracle's bid, mid and ask, json numbers verbatim
struct WireQuote {
    std::string_view pair;
    std::string_view bid;
    std::string_view mid;
    std::string_view ask;
    std::uint64_t ts_ms{0};
    bool open{false};
};

inline constexpr std::size_t k_max_quotes = 512;

struct QuoteBuffer {
    std::array<WireQuote, k_max_quotes> quotes{};
};

// a snapshot's rows, or a tick's one row
[[nodiscard]] std::expected<std::span<const WireQuote>, WireError> parse_quotes(std::string_view frame,
                                                                                QuoteBuffer& storage);

// a subgraph pair: integers scaled by 1e18, rollover per arbitrum block
struct WireRollover {
    std::string_view id;
    std::string_view long_oi;
    std::string_view short_oi;
    std::string_view long_pure;
    std::string_view premium;
    bool negative_allowed{false};
};

struct RolloverBuffer {
    std::array<WireRollover, k_max_pairs> rows{};
};

[[nodiscard]] std::expected<std::span<const WireRollover>, WireError> parse_rollover(std::string_view body,
                                                                                     RolloverBuffer& storage);

// an executed order from the subgraph; price and size scaled by 1e18
struct WireFill {
    std::string_view id; // digits on a fill, a suffixed form on a collateral edit
    std::string_view pair_id;
    std::string_view action;
    bool is_buy{false}; // the position's side, not the fill's
    std::string_view price;
    std::string_view size;
    std::uint32_t close_percent{0}; // hundredths of a percent
    std::uint64_t executed_s{0};
};

inline constexpr std::size_t k_max_fills = 1000;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

} // namespace raikiri::venues::ostium
