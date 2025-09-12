#pragma once

// What the venue says this account is long or short.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

struct WirePosition {
    std::string_view symbol;
    std::string_view side; // "Buy", "Sell", or "" when flat
    std::string_view size;
    std::string_view avg_price;
    std::uint64_t updated_ms{0};
};

// Its own, though it happens to match the order buffer's: positions used to
// borrow k_max_updates, which made a change to one silently resize the other.
inline constexpr std::size_t k_max_positions = 64;

struct PositionBuffer {
    WirePosition positions[k_max_positions];
};

// The REST shape of the same rows. TWO SHAPES, TWO PARSERS: the private socket
// wraps positions in `data` and /v5/position/list wraps them in `list`, and a
// parser pointed at the wrong key finds no array and reports an account with
// no positions — which is the one wrong answer this whole path exists to
// avoid.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_position_list(std::string_view result, PositionBuffer& storage);

[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view frame, PositionBuffer& storage);

} // namespace raikiri::venues::bybit
