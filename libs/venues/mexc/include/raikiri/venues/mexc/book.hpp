#pragma once

// The order book on both sockets: the futures depth channels, and the spot
// protobuf feed that is the only book MEXC still serves for spot.

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// depth.full offers 200 a side and the widest incremental frame observed
// carried 33; the ceiling is headroom against a resync burst rather than a
// measured maximum, and past it a frame is an attempt to make us allocate.
inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// One parser for both depth channels: the payloads are the same object, and
// `snapshot` records which channel delivered it. That distinction is load
// bearing — a push.depth delta is a set of CHANGED levels in no particular
// order (verified: a BTC_USDT delta led with a bid 1900 away from the touch),
// so only a snapshot's first entry is the touch.
//
// Continuity on the incremental channel is the version chain: a delta applies
// when its `begin` follows the previous frame's `end`. Snapshots carry
// `version` alone.
struct WireDepth {
    std::string_view symbol;
    bool snapshot{false};
    std::int64_t version{0};
    std::int64_t begin{0};
    std::int64_t end{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// The spot socket, whose payloads are protobuf submessages the envelope has
// already located. FIELD NUMBERS ARE STATED, NOT DERIVED — see envelope.hpp.

// #1 bid price, #2 bid qty, #3 ask price, #4 ask qty — all decimal strings.
struct StreamQuote {
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<StreamQuote, WireError> parse_stream_quote(std::string_view payload);

// #1 repeated asks, #2 repeated bids, each a submessage of #1 price, #2 qty.
// Both sides arrive best-first. It is a WHOLE BOOK every time: the limit-depth
// channel republishes rather than sending changes, so there is no seed to
// fetch and no chain that can silently fall behind.
struct StreamBook {
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<StreamBook, WireError> parse_stream_book(std::string_view payload,
                                                                     LevelBuffer& storage);

} // namespace raikiri::venues::mexc
