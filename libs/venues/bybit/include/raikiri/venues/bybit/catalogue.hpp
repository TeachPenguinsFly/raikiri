#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

// One entry of /v5/market/instruments-info.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    // "LinearPerpetual", "LinearFutures", "InversePerpetual", "InverseFutures".
    // The linear catalogue carries the whole futures curve beside the
    // perpetual, and only this field tells them apart.
    std::string_view contract_type;
    std::string_view tick_size;
    std::string_view qty_step;
    std::string_view min_order_qty;
    std::string_view min_notional;
    std::string_view status; // "Trading" for live instruments
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view result, InstrumentBuffer& storage);

} // namespace raikiri::venues::bybit
