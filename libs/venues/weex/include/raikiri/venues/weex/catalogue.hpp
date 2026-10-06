#pragma once

// price step is 10^-pricePrecision; size step is contractVal, since quantityPrecision can be negative
// rows carry no status and no min notional; minOrderSize is a quantity

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

struct WireInstrument {
    std::string_view symbol;
    std::string_view base_asset;
    std::string_view quote_asset;
    // PERPETUAL or TRADIFI_PERPETUAL; both settle and fund in USDT
    std::string_view contract_type;
    // COIN, Stocks, Pre-IPO, Indices, Metals, Commodities or Forex.
    std::string_view underlying_type;
    // the size step; json writes it as an int or a float
    std::string_view contract_val;
    std::uint64_t price_precision{0};
    // length of the delivery timetable; only the count is read
    std::uint32_t settlements_per_day{0};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_exchange_info(std::string_view body,
                                                                                            InstrumentBuffer& storage);

// bare array of api-tradable names, a subset of the catalogue
struct TradableBuffer {
    std::array<std::string_view, k_max_instruments> symbols{};
};

[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_api_trading_symbols(std::string_view body, TradableBuffer& storage);

// spot's GET /api/v3/exchangeInfo; it states no asset class and no minimum notional
struct WireSpotInstrument {
    std::string_view symbol;
    std::string_view status; // TRADING or HALT
    std::string_view base_asset;
    std::string_view quote_asset;
    std::string_view tick_size;
    std::string_view step_size;
};

inline constexpr std::size_t k_max_spot_instruments = 4096;

struct SpotInstrumentBuffer {
    std::array<WireSpotInstrument, k_max_spot_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireSpotInstrument>, WireError>
parse_spot_exchange_info(std::string_view body, SpotInstrumentBuffer& storage);

} // namespace raikiri::venues::weex
