#pragma once

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// the perpetual is kind future with settlement_period perpetual; there is no perpetual kind
struct WireInstrument {
    std::string_view instrument_name;
    std::string_view kind;              // future, option, spot
    std::string_view instrument_type;   // reversed, linear
    std::string_view settlement_period; // perpetual, day, week, month
    std::string_view option_type;       // call, put; empty off options
    std::string_view tick_size;
    std::string_view contract_size;
    std::string_view min_trade_amount;
    std::string_view strike; // empty off options
    std::uint64_t expiration_ms{0};
    bool is_active{false};
};

inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::deribit
