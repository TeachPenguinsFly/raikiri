#pragma once

// The order book: what a frame says about it.
//
// `depth` is a self-contained top-N snapshot on a fixed cadence, so a BBO is
// available without maintaining a book at all, and that is the only thing the
// adapter publishes from. There is no sequencing class here because nothing
// chains: `depth_update` is parsed and tested but the data client has no
// reader for it, so the u/pu rule below is documented and unenforced.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

struct WireLevel {
    std::string_view price;
    std::string_view qty; // "0" means delete this level on depth_update
};

// `depth` tops out at 50 levels a side and depth_update deltas have been seen
// at ten; the ceiling leaves room without letting a hostile frame make us
// allocate.
inline constexpr std::size_t k_max_levels = 128;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// `depth` is a self-contained top-N snapshot on a fixed cadence, which is what
// makes a BBO available without maintaining a book. `id` is XT's update id as
// a digit string.
//
// TWO TREES: futures stamps the snapshot as digit-string `id`, spot as a JSON
// number `i`, which is why this takes the market rather than guessing.
struct WireDepth {
    std::string_view symbol;
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market);

// Continuity on depth_update is the u/pu chain: a delta applies when its
// prev_update_id equals the previous message's update_id. `fu` is the first
// id folded into the delta and is always pu + 1 in every frame captured, so it
// carries no information the pair does not already give.
struct WireDepthUpdate {
    std::string_view symbol;
    std::uint64_t first_update_id{0};
    std::uint64_t prev_update_id{0};
    std::uint64_t update_id{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

[[nodiscard]] std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view frame,
                                                                           LevelBuffer& storage);

} // namespace raikiri::venues::xt
