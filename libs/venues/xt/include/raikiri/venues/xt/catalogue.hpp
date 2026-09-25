#pragma once

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// tradeSwitch and isOpenApi gate tradability; state is 0 on every row
struct WireInstrument {
    std::string_view symbol;
    std::string_view product_type; // "perpetual" | "futures"
    std::string_view tick_size;    // minStepPrice
    std::string_view contract_size;
    std::string_view min_notional;
    std::string_view tags; // raw JSON: null, ["STOCK"], ["METAL"], ["INDEX"], ...
    std::int64_t state{-1};
    std::int64_t qty_precision{-1};
    std::int64_t price_precision{-1};
    bool trade_switch{false};
    bool open_api{false};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                                          InstrumentBuffer& storage);

// precision is a digit count; the two booleans gate api trading apart from state
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view state;         // ONLINE | OFFLINE | ...
    std::string_view min_quote_qty; // filters[QUOTE_QTY].min; empty when absent
    std::int64_t price_precision{-1};
    std::int64_t qty_precision{-1};
    bool trading_enabled{false};
    bool openapi_enabled{false};
};

inline constexpr std::size_t k_max_spot_symbols = 4096;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError> parse_spot_symbols(std::string_view body,
                                                                                           SpotSymbolBuffer& storage);

} // namespace raikiri::venues::xt
