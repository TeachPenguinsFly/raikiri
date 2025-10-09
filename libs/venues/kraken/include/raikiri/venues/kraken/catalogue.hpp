#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

// `type` does NOT separate perpetual from dated: flexible_futures covers
// both PF_ and FF_, futures_inverse covers both PI_ and FI_. VERIFIED across
// all 298 live rows, `last_trading_time` is present on exactly the dated
// contracts, which is what the mapper keys on. The precision field is signed
// on purpose — see map.cpp.
struct WireInstrument {
    std::string_view symbol;
    std::string_view type;              // flexible_futures | futures_inverse
    std::string_view tick_size;         // raw numeric token
    std::string_view contract_size;     // raw numeric token
    std::string_view underlying;        // inverse families only
    std::string_view last_trading_time; // dated contracts only
    std::int64_t contract_value_trade_precision{0};
    bool tradeable{false};
    bool has_precision{false}; // absent field must not read as precision 0
};

inline constexpr std::size_t k_max_instruments = 1024;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::kraken
