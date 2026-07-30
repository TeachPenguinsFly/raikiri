#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// MEXC states futures precision as SCALES — counts of digits — never as steps.
// The derivation to a step lives in the mapper; the wire layer keeps the digit
// counts MEXC actually sent.
struct WireInstrument {
    std::string_view symbol;
    std::string_view base_coin; // the futures catalogue names both halves
    std::string_view quote_coin;
    std::string_view settle_coin; // the base coin on an inverse contract
    std::int64_t state{-1};       // 0 = enabled
    std::int64_t price_scale{-1};
    std::int64_t vol_scale{-1};
    std::string_view contract_size; // base units per contract
    std::string_view min_vol;       // contracts
    bool api_allowed{false};
};

// /contract/detail returned 1086 rows on 2026-08-10 and grows with every
// listing; past the ceiling a response is a venue incident or an attempt to
// make us allocate.
inline constexpr std::size_t k_max_instruments = 4096;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

// MEXC spot states precision as a SCALE for price (`quotePrecision`) and as a
// STEP for quantity (`baseSizePrecision`) in the same row — the two arrive in
// different forms and the mapper treats them differently. There is no
// PRICE_FILTER or LOT_SIZE in `filters`; the only filter BTCUSDT carried on
// 2026-08-26 was PERCENT_PRICE_BY_SIDE.
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_asset; // the spot symbol `BTCUSDT` states no pair
    std::string_view quote_asset;
    std::string_view status;          // "1" is trading, as a STRING
    std::string_view base_size_step;  // baseSizePrecision, a decimal step
    std::int64_t quote_precision{-1}; // price decimals
    bool spot_trading_allowed{false};
};

inline constexpr std::size_t k_max_spot_symbols = 8192;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage);

} // namespace raikiri::venues::mexc
