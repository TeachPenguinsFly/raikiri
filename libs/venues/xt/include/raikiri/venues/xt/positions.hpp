#pragma once

// What the venue says this account is long or short. Futures only: XT's spot
// tree has no position of its own.

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// `positionSize` is UNSIGNED; `positionSide` is the only thing that says which
// way a position points.
struct WireFuturesPosition {
    std::string_view symbol;
    std::string_view position_side; // LONG | SHORT
    std::string_view position_size; // CONTRACTS, unsigned
    std::string_view entry_price;
    std::string_view contract_type;
};

// A page of positions, on the same 100-per-read cap the order pages use.
//
// Its own, though it matches the order buffer's: both used to borrow one
// k_max_private_records, which made a change to either silently resize the
// other and the balance buffer beside them.
inline constexpr std::size_t k_max_positions = 512;

struct FuturesPositionBuffer {
    std::array<WireFuturesPosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage);

} // namespace raikiri::venues::xt
