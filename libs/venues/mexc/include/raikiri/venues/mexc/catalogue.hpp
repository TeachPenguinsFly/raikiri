#pragma once

#include <raikiri/venues/mexc/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

// futures precision arrives as digit scales; the mapper derives steps
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
    std::string_view concept_plate; // raw array: "mc-trade-zone-Stock", ...
    bool api_allowed{false};
};

inline constexpr std::size_t k_max_instruments = 4096;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                                          InstrumentBuffer& storage);

// price precision is a scale (`quotePrecision`), quantity a step
// (`baseSizePrecision`); no price or lot filters
struct WireSpotSymbol {
    std::string_view symbol;
    std::string_view base_asset; // the spot symbol `BTCUSDT` states no pair
    std::string_view quote_asset;
    std::string_view status;          // "1" is trading, as a string
    std::string_view base_size_step;  // baseSizePrecision, a decimal step
    std::int64_t quote_precision{-1}; // price decimals
    bool spot_trading_allowed{false};
};

inline constexpr std::size_t k_max_spot_symbols = 8192;

struct SpotSymbolBuffer {
    std::array<WireSpotSymbol, k_max_spot_symbols> symbols{};
};

[[nodiscard]] std::expected<std::span<const WireSpotSymbol>, WireError> parse_spot_symbols(std::string_view body,
                                                                                           SpotSymbolBuffer& storage);

} // namespace raikiri::venues::mexc
