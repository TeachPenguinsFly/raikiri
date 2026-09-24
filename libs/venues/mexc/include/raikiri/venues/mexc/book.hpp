#pragma once

// futures depth channels and the protobuf spot feed, the only spot book mexc serves

#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// a push.depth delta lists changed levels unordered, so only a snapshot's
// first entry is the touch; deltas chain `begin` to the previous `end`
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

struct DepthEvent {
    EventInfo event;
    std::optional<WireDepth> depth;
};

[[nodiscard]] std::expected<DepthEvent, WireError> parse_event_and_depth(std::string_view frame,
                                                                         LevelBuffer& storage);

// #1 bid price, #2 bid qty, #3 ask price, #4 ask qty, all decimal strings
struct StreamQuote {
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
};

[[nodiscard]] std::expected<StreamQuote, WireError> parse_stream_quote(std::string_view payload);

// #1 asks, #2 bids, each #1 price and #2 qty, best first; a whole book every frame
struct StreamBook {
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<StreamBook, WireError> parse_stream_book(std::string_view payload,
                                                                     LevelBuffer& storage);

} // namespace raikiri::venues::mexc
