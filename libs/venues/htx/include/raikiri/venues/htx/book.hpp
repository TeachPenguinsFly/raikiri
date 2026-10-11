#pragma once

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

// swap: bid/ask are [price, contracts]; spot: bid/ask are prices beside bidSize/askSize in base
// tick.ts (spot quoteTime) is engine time and the root ts is send time; spot's seqId lands in version
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

inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// full snapshot every frame; swap's version is epoch seconds, so mrid orders frames; spot has no mrid and its version
// is a sequence
struct WireDepth {
    std::string_view contract_code;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
    std::uint64_t mrid{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage);

} // namespace raikiri::venues::htx
