#pragma once

// What the venue says this account is long or short.

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// `position_type` is 1 long / 2 short and `hold_vol` is UNSIGNED, so the sign
// of a net position lives in the type field alone.
struct WireFuturesPosition {
    std::string_view symbol;
    std::string_view hold_vol;
    std::string_view hold_avg_price;
    std::int64_t position_type{0};
    std::int64_t state{0}; // 1 holding, 2 system auto-holding, 3 closed
    std::uint64_t update_ms{0};
};

// Its own, though it happens to match the order buffers': positions used to
// borrow k_max_private_records, which made a change to one silently resize
// the other.
inline constexpr std::size_t k_max_positions = 512;

struct FuturesPositionBuffer {
    std::array<WireFuturesPosition, k_max_positions> positions{};
};

// `data` as an ARRAY (the REST list endpoint).
[[nodiscard]] std::expected<std::span<const WireFuturesPosition>, WireError>
parse_futures_positions(std::string_view body, FuturesPositionBuffer& storage);

// `data` as a single OBJECT — the socket push.
[[nodiscard]] std::expected<WireFuturesPosition, WireError>
parse_futures_position(std::string_view frame);

} // namespace raikiri::venues::mexc
