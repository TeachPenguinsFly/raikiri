#include <raikiri/portfolio/pnl.hpp>

namespace raikiri::portfolio {
namespace {

// Quantity in the venue's own units (contracts, or base units where the venue
// counts those), scaled by 1e9.
[[nodiscard]] std::int64_t venue_units_e9(const InstrumentBase& base, QtyLots qty) noexcept {
    return detail::checked_mul(qty.raw, base.lot_size.raw);
}

[[nodiscard]] std::int64_t price_e9(const InstrumentBase& base, PriceTicks price) noexcept {
    return detail::checked_mul(price.raw, base.tick_size.raw);
}

} // namespace

FixedE9 base_quantity(const InstrumentBase& base, QtyLots qty) noexcept {
    if (base.kind != ContractKind::Linear) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(venue_units_e9(base, qty), base.contract_value.raw, k_e9));
}

FixedE9 settlement_value(const InstrumentBase& base, PriceTicks price, QtyLots qty) noexcept {
    const auto units = venue_units_e9(base, qty);
    const auto px = price_e9(base, price);
    if (base.kind == ContractKind::Inverse) {
        // A zero price would mean the venue quoted a contract as worthless.
        // Refusing beats dividing.
        if (px <= 0) {
            return FixedE9::undef();
        }
        return FixedE9::from_raw(mul_div(units, base.contract_value.raw, px));
    }
    const auto base_qty = mul_div(units, base.contract_value.raw, k_e9);
    return FixedE9::from_raw(mul_div(base_qty, px, k_e9));
}

} // namespace raikiri::portfolio
