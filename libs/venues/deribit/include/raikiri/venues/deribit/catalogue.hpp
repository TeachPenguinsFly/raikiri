#pragma once

// What the venue lists, and the numbers that decide whether an order it
// accepts is an order it will fill.

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// One /public/get_instruments row; numeric fields are raw tokens.
//
// kind + settlement_period together decide the market: BTC-PERPETUAL is kind
// "future" with settlement_period "perpetual", because there is no "perpetual"
// kind — asking for one is an Invalid params error (verified). instrument_type
// decides what contract_size COUNTS, which is the difference between a correct
// notional and one off by the price of BTC.
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

// 794 live BTC options the day this was written. The catalogue is pulled one
// currency at a time so this tracks a strike ladder, not the whole universe.
inline constexpr std::size_t k_max_instruments = 2048;

struct InstrumentBuffer {
    std::array<WireInstrument, k_max_instruments> instruments{};
};

[[nodiscard]] std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage);

} // namespace raikiri::venues::deribit
