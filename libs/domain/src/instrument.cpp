#include <raikiri/domain/instrument.hpp>

namespace raikiri {

namespace {

std::expected<std::int64_t, ConvError> exact_div(FixedE9 value, FixedE9 step) noexcept {
    if (value.is_undef() || step.is_undef()) {
        return std::unexpected{ConvError::UndefValue};
    }
    if (step.raw <= 0) {
        return std::unexpected{ConvError::ZeroStep};
    }
    if (value.raw % step.raw != 0) {
        return std::unexpected{ConvError::NotAMultiple};
    }
    return value.raw / step.raw;
}

} // namespace

std::expected<PriceTicks, ConvError> InstrumentBase::price_to_ticks(FixedE9 price) const noexcept {
    return exact_div(price, tick_size).transform(PriceTicks::from_raw);
}

std::expected<QtyLots, ConvError> InstrumentBase::qty_to_lots(FixedE9 qty) const noexcept {
    return exact_div(qty, lot_size).transform(QtyLots::from_raw);
}

FixedE9 InstrumentBase::ticks_to_price(PriceTicks ticks) const noexcept {
    if (ticks.is_undef()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(detail::checked_mul(ticks.raw, tick_size.raw));
}

FixedE9 InstrumentBase::lots_to_qty(QtyLots lots) const noexcept {
    if (lots.is_undef()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(detail::checked_mul(lots.raw, lot_size.raw));
}

bool validate(const InstrumentBase& base) noexcept {
    return base.id.valid() && !base.local_symbol.empty() && !base.exchange_symbol.empty() &&
           !base.tick_size.is_undef() && base.tick_size.raw > 0 && !base.lot_size.is_undef() &&
           base.lot_size.raw > 0 && !base.contract_value.is_undef() && base.contract_value.raw > 0;
}

} // namespace raikiri
