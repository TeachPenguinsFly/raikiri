#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

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
    std::string_view symbol_type; // "", "innovation", "stock", "ETF", "commodity", "forex"
    std::string_view tick_size;
    std::string_view qty_step;
    std::string_view min_order_qty;
    std::string_view min_notional;
    // MINUTES, AND A JSON NUMBER — the one field here that is not a digit
    // string. 0 is the venue declining to state it, which is not the same as
    // eight hours: 40 rows carry it today, and a board that reads 0 as a
    // default settles an edge against a schedule nobody published.
    std::uint64_t funding_interval_min{0};
    // upperFundingRate / lowerFundingRate: the per-interval rate bybit will
    // not charge beyond. Not uniform — BTC and ETH are held to 0.333% while
    // most of the catalogue sits at 2%.
    std::string_view funding_cap;
    std::string_view funding_floor;
    std::string_view status; // "Trading" for live instruments
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view result, InstrumentBuffer& storage);

} // namespace raikiri::venues::bybit
