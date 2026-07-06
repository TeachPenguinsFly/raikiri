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

#include <cstddef>
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

// Which PnL formula an instrument obeys, and therefore what `contract_value`
// below actually means. Nothing else distinguishes them: three venues express
// the same perpetual three ways and a BTC-USD-SWAP priced as if it were
// BTC-USDT-SWAP is wrong by the price, not by a rounding.
//
// Unknown is 0 so a zero-initialised InstrumentBase fails validate() instead of
// defaulting to something plausible. An adapter that cannot tell from the
// catalogue row refuses the row.
enum class ContractKind : std::uint8_t {
    Unknown = 0,
    Linear,  // contract_value is BASE units per contract; PnL settles in quote
    Inverse, // contract_value is QUOTE units per contract; PnL settles in BASE
};

// What the contract tracks. Unknown pairs with nothing, including another
// Unknown: an unclassified listing is not evidence of the same underlying.
enum class AssetClass : std::uint8_t {
    Unknown = 0,
    Crypto,
    Equity,
    Index,
    Commodity,
    FX,
};

using UnderlyingText = FixedString<15>;

struct InstrumentBase {
    InstrumentId id{};
    SymbolText local_symbol{};                // canonical, venue-neutral: "BTC/USDT"
    SymbolText exchange_symbol{};             // venue wire form: "BTCUSDT"
    ContractKind kind{ContractKind::Unknown}; // rides padding; see the asserts below
    AssetClass asset_class{AssetClass::Unknown};
    UnderlyingText underlying{}; // venue-neutral key within asset_class: "TSLA", "XAU"
    FixedE9 tick_size{};                      // price step, decimal e9
    FixedE9 lot_size{};                       // qty step, decimal e9
    FixedE9 min_notional{};
    FixedE9 contract_value{}; // units per contract; `kind` says WHICH units
    FixedE9 maker_fee{};
    FixedE9 taker_fee{};
    UnixNanos ts_loaded{};

    [[nodiscard]] std::expected<PriceTicks, ConvError> price_to_ticks(FixedE9 price) const noexcept;
    [[nodiscard]] std::expected<QtyLots, ConvError> qty_to_lots(FixedE9 qty) const noexcept;
    [[nodiscard]] FixedE9 ticks_to_price(PriceTicks ticks) const noexcept;
    [[nodiscard]] FixedE9 lots_to_qty(QtyLots lots) const noexcept;
};

// `kind` and `asset_class` ride padding, so only `underlying` moved sizeof —
// k_abi_version (shm/segment.hpp) is what catches a mixed-build pair. Any
// future field that trips these asserts is an ABI decision, made deliberately.
static_assert(sizeof(InstrumentBase) == 136);
static_assert(offsetof(InstrumentBase, kind) == 58);
static_assert(offsetof(InstrumentBase, asset_class) == 59);
static_assert(offsetof(InstrumentBase, tick_size) == 80);

struct SpotInstrument {
    InstrumentBase base{};
};

struct PerpInstrument {
    InstrumentBase base{};
    std::uint32_t funding_interval_s{0};
    // WHAT THE VENUE WILL NOT CHARGE BEYOND. A rate sitting exactly on its cap
    // is a different fact from a rate that happens to be high: the first is
    // demand the venue stopped pricing, so the spread it implies is wider than
    // the rate says and the next settlement is no evidence about the one
    // after. UNDEF where a venue does not publish them, which is most of them
    // — a cap assumed is a cap that will be compared against.
    FixedE9 funding_cap{};
    FixedE9 funding_floor{};
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

// The variant is sized by OptionInstrument, so a perpetual's funding caps cost
// nothing in a catalogue cell and no segment ABI moved to carry them. Stated
// rather than left to luck: a field added past this grows every cell.
static_assert(sizeof(PerpInstrument) <= sizeof(OptionInstrument));

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
