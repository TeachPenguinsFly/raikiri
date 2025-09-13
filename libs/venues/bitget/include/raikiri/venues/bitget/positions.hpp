#pragma once

// What the venue says this account is long or short.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// `holdSide` names the direction; there is no signed size anywhere.
struct WirePosition {
    std::string_view symbol; // `instId` on the socket, `symbol` over REST
    std::string_view hold_side;
    std::string_view total;
    std::string_view open_price_avg;
    std::uint64_t update_ms{0};
};

// Its own, though it happens to match the order buffer's: positions used to
// borrow k_max_updates, which made a change to one silently resize the other.
inline constexpr std::size_t k_max_positions = 128;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::bitget
