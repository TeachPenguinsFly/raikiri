#pragma once

// What the venue says this account is long or short.
//
// ONE SYMBOL CAN CARRY SEVERAL ROWS. WEEX runs positions in one of two modes —
// `separatedMode` COMBINED merges the two directions into one row, SEPARATED
// keeps a row per direction with its own margin and its own leverage — and the
// account-wide read answers with whatever the account is set to. Every consumer
// of a position report keys on the INSTRUMENT, so two rows arriving for one
// contract means the second overwrites the first and the desk is told it holds
// the short leg only. They are netted before they leave this adapter.
//
// THERE IS NO ENTRY PRICE ON A ROW. WEEX states `openValue` and `size` and
// nothing between them; the quotient is not a price the venue ever quoted and
// need not land on the contract's tick, so `avg_entry` stays UNDEF rather than
// carrying a division.

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

struct WirePosition {
    std::string_view symbol;
    // LONG or SHORT. `size` is UNSIGNED, so this is the only thing that says
    // which way the account is facing.
    std::string_view side;
    std::string_view size;
    std::uint64_t updated_ms{0};
};

// Its own constant rather than the order buffer's: a desk long a hundred
// contracts is ordinary, and a change to one list must not silently resize the
// other.
inline constexpr std::size_t k_max_positions = 256;

struct PositionBuffer {
    std::array<WirePosition, k_max_positions> positions{};
};

// GET /capi/v3/account/position/allPosition answers a BARE ARRAY, and so does
// the single-symbol read beside it.
[[nodiscard]] std::expected<std::span<const WirePosition>, WireError>
parse_positions(std::string_view body, PositionBuffer& storage);

} // namespace raikiri::venues::weex
