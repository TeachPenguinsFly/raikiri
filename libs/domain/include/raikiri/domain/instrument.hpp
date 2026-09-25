#pragma once

// a wire value that does not divide by tick or lot size is a schema event and is never rounded

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

// says what contract_value means; unknown is 0 so a zeroed instrument fails validate()
enum class ContractKind : std::uint8_t {
    Unknown = 0,
    Linear,  // contract_value is base units per contract; pnl settles in quote
    Inverse, // contract_value is quote units per contract; pnl settles in base
};

// unknown pairs with nothing, including another unknown
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
    FixedE9 tick_size{};         // price step, decimal e9
    FixedE9 lot_size{};          // qty step, decimal e9
    FixedE9 min_notional{};
    FixedE9 contract_value{}; // units per contract; `kind` says which units
    FixedE9 maker_fee{};
    FixedE9 taker_fee{};
    UnixNanos ts_loaded{};

    [[nodiscard]] std::expected<PriceTicks, ConvError> price_to_ticks(FixedE9 price) const noexcept;
    [[nodiscard]] std::expected<QtyLots, ConvError> qty_to_lots(FixedE9 qty) const noexcept;
    [[nodiscard]] FixedE9 ticks_to_price(PriceTicks ticks) const noexcept;
    [[nodiscard]] FixedE9 lots_to_qty(QtyLots lots) const noexcept;
};

// kind and asset_class ride padding; changing these asserts is an abi change (k_abi_version)
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
    // the venue's rate cap; undef where the venue does not publish one
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

static_assert(sizeof(PerpInstrument) <= sizeof(OptionInstrument));

using Instrument = std::variant<SpotInstrument, PerpInstrument, FutureInstrument, OptionInstrument>;

[[nodiscard]] inline const InstrumentBase& base_of(const Instrument& instrument) noexcept {
    return std::visit([](const auto& i) -> const InstrumentBase& { return i.base; }, instrument);
}

[[nodiscard]] inline MarketType market_of(const Instrument& instrument) noexcept {
    return base_of(instrument).id.market();
}

[[nodiscard]] bool validate(const InstrumentBase& base) noexcept;

} // namespace raikiri
