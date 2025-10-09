#pragma once

// Top of book and the depth ladder, both off the swap market socket.
//
// There is no delta chain here and so no sequencing class: step0 is a
// complete snapshot every frame.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// `bid`/`ask` are two-element arrays: price then a count of CONTRACTS, never
// base units. tick.ts is the matching-engine time; the root `ts` is when HTX
// sent the frame.
struct WireBbo {
    std::string_view contract_code;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// step0 is the unaggregated book and ships about 150 levels a side; the
// ceiling leaves room without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// A complete snapshot every time, with no delta chain and so no continuity to
// validate. `version` equals tick.id on step0 and tick.mrid on the aggregated
// levels, which is why it is carried rather than trusted as a sequence.
//
// RE-VERIFIED 2026-08-17 on BTC-USDT: 119 consecutive step0 frames, every one
// of them 150 levels a side with no action field anywhere. `version` and `id`
// are epoch SECONDS, so ten frames a second share one value; `mrid` is the
// matching-engine id and is the only token here that orders frames.
struct WireDepth {
    std::string_view contract_code;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
    std::uint64_t mrid{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

} // namespace raikiri::venues::htx
