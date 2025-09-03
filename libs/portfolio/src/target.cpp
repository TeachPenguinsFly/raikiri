#include <raikiri/portfolio/target.hpp>

namespace raikiri::portfolio {

Rebalance delta_to_target(QtyLots target,
                          QtyLots position,
                          QtyLots working_buy,
                          QtyLots working_sell) noexcept {
    const auto projected =
        detail::checked_add(position.raw, detail::checked_sub(working_buy.raw, working_sell.raw));
    const auto missing = detail::checked_sub(target.raw, projected);
    if (missing == 0) {
        return {};
    }
    return {missing > 0 ? Side::Buy : Side::Sell,
            QtyLots::from_raw(missing > 0 ? missing : -missing)};
}

std::optional<QtyLots> size_for_risk(const InstrumentBase& base,
                                     FixedE9 equity,
                                     FixedE9 risk_fraction,
                                     PriceTicks entry,
                                     PriceTicks stop) noexcept {
    if (equity.is_undef() || equity.raw <= 0 || risk_fraction.is_undef() ||
        risk_fraction.raw <= 0 || entry.is_undef() || stop.is_undef()) {
        return std::nullopt;
    }
    if (entry.raw == stop.raw) {
        return std::nullopt; // a stop at the entry risks everything or nothing
    }

    // What one lot loses over the stop distance, measured through the same
    // function that prices everything else — so an inverse contract is sized
    // by its base-denominated loss and not by a quote-shaped guess.
    const auto one = QtyLots::from_raw(1);
    const auto at_entry = settlement_value(base, entry, one);
    const auto at_stop = settlement_value(base, stop, one);
    if (at_entry.is_undef() || at_stop.is_undef()) {
        return std::nullopt;
    }
    const auto per_lot =
        at_entry.raw > at_stop.raw ? at_entry.raw - at_stop.raw : at_stop.raw - at_entry.raw;
    if (per_lot <= 0) {
        return std::nullopt; // the stop is inside a tick: no measurable risk
    }

    const auto budget = mul_div(equity.raw, risk_fraction.raw, k_e9);
    const auto lots = budget / per_lot; // truncates: never round a size UP
    return QtyLots::from_raw(lots);
}

} // namespace raikiri::portfolio
