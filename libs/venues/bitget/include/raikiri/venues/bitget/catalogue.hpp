#pragma once

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// one /api/v2/mix/market/contracts row; neither price field alone is the tick
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

inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                                      ContractBuffer& storage);

// spot states precision as decimal places and has no priceEndStep
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

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError> parse_spot_symbols(std::string_view body,
                                                                                           SpotSymbolBuffer& storage);

} // namespace raikiri::venues::bitget
