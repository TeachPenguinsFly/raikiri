#pragma once

// What a quantity of an instrument is worth, and what a closed trade earned.
//
// LEVERAGE IS NOT A TERM IN ANY FORMULA IN THIS FILE, and adding one is always
// a bug. Leverage sets the margin a venue demands against a position; it does
// not change what the position is worth. A recorded incident in the sibling Go
// system booked $0.35 of PnL as $17.56 because a leverage multiplier reached
// arithmetic like this.
//
// Everything here is a pure function. Nothing caches, nothing is stored, and
// unrealized PnL in particular is a function of a mark price passed in as an
// argument — a stored unrealized field can only answer "as of when?" wrongly.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>

namespace raikiri::portfolio {

// Which currency the numbers below are denominated in. Derived from the
// instrument, never stored beside it: it is not independent information.
enum class SettleSide : std::uint8_t {
    Quote = 0, // linear: BTC/USDT PnL is in USDT
    Base,      // inverse: XBTUSD PnL is in BTC, and USD is only a pricing unit
};

[[nodiscard]] constexpr SettleSide settle_side(ContractKind kind) noexcept {
    return kind == ContractKind::Inverse ? SettleSide::Base : SettleSide::Quote;
}

// Value of `qty` at `price`, in the instrument's settlement currency.
//
//   linear   qty x lot x contract_value  base units, times the price  -> quote
//   inverse  qty x lot x contract_value  QUOTE units, divided by price -> base
//
// The linear path goes through base quantity rather than through a
// pre-contract notional on purpose: base quantity is bounded by the size of a
// real position, while a notional taken before the contract multiplier is
// applied is inflated by 1/contract_value and overflows for the sub-unit
// multipliers gate and bitmex publish.
[[nodiscard]] FixedE9
settlement_value(const InstrumentBase& base, PriceTicks price, QtyLots qty) noexcept;

// Position size expressed in base units, whatever the instrument counts in.
// Meaningless for an inverse contract, whose base exposure floats with price,
// so it is refused there rather than approximated.
[[nodiscard]] FixedE9 base_quantity(const InstrumentBase& base, QtyLots qty) noexcept;

} // namespace raikiri::portfolio
