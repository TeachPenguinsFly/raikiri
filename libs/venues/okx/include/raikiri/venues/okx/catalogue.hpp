#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// ct_val/ct_mult are empty on SPOT rows and on rows that are not live yet.
struct WireInstrument {
    std::string_view inst_id;
    std::string_view inst_type; // SWAP, FUTURES, SPOT, OPTION
    std::string_view state;     // live, preopen, suspend, expired
    std::string_view base_ccy;  // SPOT only; empty on SWAP, FUTURES and OPTION
    std::string_view quote_ccy; // SPOT only; empty on SWAP, FUTURES and OPTION
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view min_size;
    std::string_view ct_val;
    std::string_view ct_mult;
    std::string_view ct_type; // linear | inverse; empty on SPOT and OPTION rows
};

// Sized for the SPOT catalogue, which is roughly triple the swap one: 1383
// rows live today against 457 swaps. Sized for the swaps alone, a spot
// gateway refused its whole catalogue with TooManyEntries and started with
// nothing. The buffer is static thread_local, so this is one allocation for
// the process and not a stack frame.
inline constexpr std::size_t k_max_instruments = 4096;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::okx
