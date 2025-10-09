#pragma once

// Top of book and the depth ladder, on both trees.
//
// There is no delta chain here and so no sequencing class: every depth frame
// is a complete snapshot of the top N levels.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// `T` is when the book changed, `E` when BingX sent it — the only latency
// measurement this channel offers.
struct WireBookTicker {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    // Swap only. The spot frame carries neither, so both stay zero there and
    // nothing downstream may treat zero as a real sequence or timestamp.
    std::uint64_t update_id{0};
    std::uint64_t transaction_ms{0};
    std::uint64_t event_ms{0};
};

[[nodiscard]] std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// The depth channels top out at 100 levels a side, and the ceiling leaves room
// without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// A complete snapshot of the top N levels with NO sequence number and no delta
// chain, so there is no continuity to validate. `ts` sits beside `data`, and
// the payload never names the symbol — that comes from `dataType`.
// Both sides come out BEST FIRST, whatever the wire did. That normalisation is
// the reason this takes a market: the swap tree sends asks ascending, and the
// spot tree sends them DESCENDING, so a spot book read the swap way puts the
// worst offer at the top — a hundred dollars wide on BTC and inverted against
// its own bid.
struct WireDepth {
    std::string_view symbol;
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids; // descending: best bid first
    std::span<const WireLevel> asks; // ascending: best ask first
};

[[nodiscard]] std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market);

} // namespace raikiri::venues::bingx
