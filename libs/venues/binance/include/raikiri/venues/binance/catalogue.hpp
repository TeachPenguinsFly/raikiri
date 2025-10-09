#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// The three sizes come off the row's `filters` array rather than off named
// fields, which is why the reader walks it once per row.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset; // binance names both halves on every row
    std::string_view quote_asset;
    std::string_view contract_type; // PERPETUAL, CURRENT_QUARTER, ...
    std::string_view status;        // TRADING, SETTLING, PENDING_TRADING, ...
    std::string_view tick_size;
    std::string_view step_size;
    std::string_view min_notional;
    std::uint64_t delivery_ms{0};
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::binance
