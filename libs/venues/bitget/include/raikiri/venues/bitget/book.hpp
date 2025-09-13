#pragma once

// The order book: the level rows and the chain that says whether a delta may
// be applied at all.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level
};

// `books` snapshots 500 levels a side; the ceiling leaves room without
// letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 1024;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// Continuity is the seq chain: an update applies when its prev_seq_id equals
// the previous message's seq_id. books15 restates the whole top of book and
// pins prev_seq_id to 0, so only the incremental `books` channel has a chain
// worth checking — 0 for "no predecessor", where okx uses -1.
struct WireBooks {
    std::string_view inst_id;
    bool snapshot{false}; // action: "snapshot" vs "update"
    std::int64_t seq_id{0};
    std::int64_t prev_seq_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireBooks, WireError> parse_books(std::string_view frame,
                                                              LevelBuffer& storage);

// /api/v2/mix/market/orderbook. Unlike every other endpoint here its `data`
// is an OBJECT, not a single-element array, so it needs its own reader.
[[nodiscard]] std::expected<WireBooks, WireError> parse_rest_orderbook(std::string_view body,
                                                                       LevelBuffer& storage);

} // namespace raikiri::venues::bitget
