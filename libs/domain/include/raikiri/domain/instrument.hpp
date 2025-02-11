#pragma once

// Instrument model. Spot and derivatives are symmetric first-class market
// types; OptionInstrument is a member of the variant on purpose — every
// visitor is forced by the compiler to consider it today, even though
// execution routing rejects options until an options-capable venue adapter
// lands. That is the documented extension seam.
//
// Prices and quantities cross this boundary exactly once each direction:
//   wire decimal -> FixedE9 -> ticks/lots   (parse edge; must divide exactly)
//   ticks/lots  -> FixedE9  -> wire decimal (serialize edge)
// A wire value that doesn't divide by the instrument's tick/lot size is a
// schema event (venue changed its filters), never something to round over.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <expected>
#include <variant>

namespace raikiri {

using SymbolText = FixedString<24>;

enum class ConvError : std::uint8_t {
    UndefValue,
    NotAMultiple, // wire value doesn't divide by tick/lot size
    ZeroStep,     // instrument carries no tick/lot size (bad load)
};

struct InstrumentBase {
    InstrumentId id{};
    SymbolText local_symbol{};    // canonical, venue-neutral: "BTC/USDT"
    SymbolText exchange_symbol{}; // venue wire form: "BTCUSDT"
    FixedE9 tick_size{};          // price step, decimal e9
    FixedE9 lot_size{};           // qty step, decimal e9
    FixedE9 min_notional{};
    FixedE9 contract_value{}; // qty multiplier for derivatives; 1.0 for spot
    FixedE9 maker_fee{};
    FixedE9 taker_fee{};
    UnixNanos ts_loaded{};

    [[nodiscard]] std::expected<PriceTicks, ConvError> price_to_ticks(FixedE9 price) const noexcept;
    [[nodiscard]] std::expected<QtyLots, ConvError> qty_to_lots(FixedE9 qty) const noexcept;
    [[nodiscard]] FixedE9 ticks_to_price(PriceTicks ticks) const noexcept;
    [[nodiscard]] FixedE9 lots_to_qty(QtyLots lots) const noexcept;
};

struct SpotInstrument {
    InstrumentBase base{};
};

struct PerpInstrument {
    InstrumentBase base{};
    std::uint32_t funding_interval_s{0};
};

struct FutureInstrument {
    InstrumentBase base{};
    UnixNanos expiry{};
};

enum class PutCall : std::uint8_t { Put = 0, Call };

struct OptionInstrument {
    InstrumentBase base{};
    PriceTicks strike{};
    UnixNanos expiry{};
    PutCall put_call{PutCall::Call};
};

using Instrument = std::variant<SpotInstrument, PerpInstrument, FutureInstrument, OptionInstrument>;

[[nodiscard]] inline const InstrumentBase& base_of(const Instrument& instrument) noexcept {
    return std::visit([](const auto& i) -> const InstrumentBase& { return i.base; }, instrument);
}

[[nodiscard]] inline MarketType market_of(const Instrument& instrument) noexcept {
    return base_of(instrument).id.market();
}

// Validation applied when an instrument table is loaded (from Postgres or the
// bundled demo instruments.json). Loud failure at load beats a quiet zero
// step size dividing an order price at 2am.
[[nodiscard]] bool validate(const InstrumentBase& base) noexcept;

} // namespace raikiri
