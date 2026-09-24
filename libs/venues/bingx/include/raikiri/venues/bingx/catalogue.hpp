#pragma once

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// precisions are digit counts; map.cpp derives the steps and ignores size
// minimums are raw tokens since bingx may send them in scientific notation
struct WireContract {
    std::string_view symbol;
    std::string_view size;
    std::string_view trade_min_quantity;
    std::string_view trade_min_usdt;
    std::int64_t status{0}; // 1 = online; 25 = a listed-but-not-trading row
    std::int64_t price_precision{0};
    std::int64_t quantity_precision{0};
    // apiStateOpen/apiStateClose as strings "true"/"false"; only order entry checks them
    bool api_open{true};
    bool api_close{true};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// tickSize/stepSize are raw tokens, often scientific; a step finer than 1e-9 is refused
// status 1 is online; apiStateBuy/apiStateSell gate api orders separately
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::int64_t status{0};
    bool api_buy{false};
    bool api_sell{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::bingx
