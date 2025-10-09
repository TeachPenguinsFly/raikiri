#pragma once

// What the venue lists, on both trees: perpetual contracts, which state digit
// counts and leave map.cpp to derive the steps, and spot pairs, which state
// the steps outright.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// BingX states PRECISIONS — counts of decimal digits — where every other venue
// here states a tick and a lot size; map.cpp derives the steps from them.
//
// `size` looks like a quantity step and is not one: it matches 10^-quantity
// _precision on 1017 of 1021 live rows and disagrees on the rest (SOL-USDT says
// "1" at a precision of 2, and live SOL quantities carry two decimals). Carried
// so a row that contradicts the precision is visible; nothing maps it.
//
// The minimums are raw JSON tokens because BingX emits them as NUMBERS, and
// tradeMinQuantity sometimes in scientific notation ("7e-05"), which
// parse_decimal_e9 refuses — better than failing a catalogue over formatting.
struct WireContract {
    std::string_view symbol;
    std::string_view size;
    std::string_view trade_min_quantity;
    std::string_view trade_min_usdt;
    std::int64_t status{0}; // 1 = online; 25 = a listed-but-not-trading row
    std::int64_t price_precision{0};
    std::int64_t quantity_precision{0};
};

inline constexpr std::size_t k_max_instruments = 2048; // 1021 rows live today

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// /openApi/spot/v1/common/symbols states a tickSize and a stepSize outright,
// where the swap tree states digit counts and map.cpp derives the steps. Both
// arrive as JSON NUMBERS and often in scientific notation ("1e-06", and 1e-12
// on nine live rows), so they are carried as raw tokens and read by the
// exponent-aware parser; a step finer than 1e-9 has no exact FixedE9 and the
// row is refused rather than rounded into a grid the venue does not use.
//
// `status` is 1 for online and 0, 10 or 25 otherwise (VERIFIED across 2258
// live rows). `apiStateBuy`/`apiStateSell` are a SECOND gate: 36 of the 705
// online pairs are closed to API orders in at least one direction.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::int64_t status{0};
    bool api_buy{false};
    bool api_sell{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096; // 2258 rows live today

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::bingx
