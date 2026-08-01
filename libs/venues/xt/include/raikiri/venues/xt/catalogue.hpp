#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill. Two trees, two rows: the futures contract
// and the spot symbol agree only that precision arrives as a DIGIT COUNT.

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// XT states the price step outright (minStepPrice) but only a digit count for
// quantity, so the two arrive in different forms and the mapper treats them
// differently. state/precision/the switches are JSON numbers and booleans.
//
// The switches are the real tradability gate: `state` was 0 on all 1067 rows
// in the live catalogue, while tradeSwitch and isOpenApi split it roughly in
// half. A row that is not open to the API cannot be traded by this system even
// though XT still lists and prices it.
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

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// XT states spot precision as DIGIT COUNTS, like the futures catalogue, so the
// step is derived the same way. `state` is the text "ONLINE"; the two booleans
// beside it are the real gate — a pair listed and priced for the web UI is not
// necessarily open to the API.
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

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::xt
