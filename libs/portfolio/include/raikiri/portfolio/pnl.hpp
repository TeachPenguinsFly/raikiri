#pragma once

// leverage is not a term in any formula here; it sets margin, not value

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>

namespace raikiri::portfolio {

// derived from the instrument, never stored
enum class SettleSide : std::uint8_t {
    Quote = 0, // linear: BTC/USDT PnL is in USDT
    Base,      // inverse: XBTUSD pnl is in BTC
};

[[nodiscard]] constexpr SettleSide settle_side(ContractKind kind) noexcept {
    return kind == ContractKind::Inverse ? SettleSide::Base : SettleSide::Quote;
}

// an option is counted in its premium currency, whatever its contract kind says
[[nodiscard]] SettleSide settle_side(const Instrument& instrument) noexcept;

// a fee joins a position's sums only in the currency they are counted in; nothing here can price another
[[nodiscard]] bool fee_joins_sums(const InstrumentBase& base, SettleSide side, const FeeAsset& asset) noexcept;

// an inverse option is still premium times size: only its currency is the coin
[[nodiscard]] constexpr bool values_inverse(const InstrumentBase& base) noexcept {
    return base.kind == ContractKind::Inverse && base.id.market() != MarketType::Option;
}

// linear goes through base quantity so sub-unit contract values do not overflow
[[nodiscard]] FixedE9 settlement_value(const InstrumentBase& base, PriceTicks price, QtyLots qty) noexcept;

// refused for inverse contracts, whose base exposure floats with price
[[nodiscard]] FixedE9 base_quantity(const InstrumentBase& base, QtyLots qty) noexcept;

} // namespace raikiri::portfolio
