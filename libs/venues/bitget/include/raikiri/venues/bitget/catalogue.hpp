#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// One /api/v2/mix/market/contracts row. Neither price field alone is the
// tick — see map.cpp.
struct WireContract {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    std::string_view symbol_type;   // perpetual, delivery
    std::string_view symbol_status; // normal, maintain, limit_open, off, ...
    std::string_view price_place;
    std::string_view price_end_step;
    std::string_view size_multiplier; // the quantity step
    std::string_view min_trade_num;   // a lot floor, not a notional one
    std::string_view min_trade_usdt;
    std::string_view fund_interval; // hours: 1, 4 and 8 are all live
};

// 745 USDT-FUTURES rows on 2026-08-11 and the list only grows.
inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// One /api/v2/spot/public/symbols row. Spot states precision as DECIMAL
// PLACES and ships no priceEndStep at all, so the mix row's two-factor tick
// has no analogue here — reading the two catalogues through one struct is how
// a spot tick silently gains a factor it never had.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_coin;
    std::string_view quote_coin;
    std::string_view status; // online, halt, offline
    std::string_view price_precision;
    std::string_view quantity_precision;
    std::string_view min_trade_amount;
    std::string_view min_trade_usdt;
};

// 1280 spot rows on 2026-08-21 against 745 mix rows, and both only grow.
inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::bitget
