#include <raikiri/portfolio/position.hpp>

#include <algorithm>

namespace raikiri::portfolio {
namespace {

[[nodiscard]] std::int64_t magnitude(std::int64_t v) noexcept {
    return v < 0 ? -v : v;
}

// Settlement value carried into the book with the sign of the trade: a buy
// adds what it cost, a sell adds what it returned as a negative. Closing then
// needs no side-specific formula — the two signs cancel on their own.
[[nodiscard]] std::int64_t
signed_value(const InstrumentBase& base, Side side, QtyLots qty, PriceTicks price) noexcept {
    const auto value = settlement_value(base, price, qty);
    if (value.is_undef()) {
        return 0;
    }
    return side == Side::Buy ? value.raw : -value.raw;
}

} // namespace

FixedE9 Position::net_realized() const noexcept {
    return FixedE9::from_raw(detail::checked_sub(realized_.raw, fees_.raw));
}

Position::Applied Position::apply_fill(const InstrumentBase& base,
                                       Side side,
                                       QtyLots delta,
                                       PriceTicks price,
                                       FixedE9 fee,
                                       UnixNanos ts) noexcept {
    Applied out{};
    if (delta.is_undef() || delta.raw <= 0) {
        return out; // a resent notification derives a zero delta; nothing happened
    }
    out.open_fee = fee;

    const std::int64_t open_qty = qty_.raw;
    const bool opposes = (open_qty > 0 && side == Side::Sell) || (open_qty < 0 && side == Side::Buy);
    const std::int64_t close_raw = opposes ? std::min(magnitude(open_qty), delta.raw) : 0;
    const std::int64_t add_raw = delta.raw - close_raw;
    out.flipped = close_raw > 0 && add_raw > 0;

    if (close_raw > 0) {
        // Split BEFORE any arithmetic runs, so the sign never crosses zero
        // inside the basis math. One rounding, and the other half by
        // subtraction: rounding both independently makes the venue's fee and
        // the sum of our attributions disagree, and the gap compounds.
        out.close_fee = close_raw == delta.raw
                            ? fee
                            : FixedE9::from_raw(mul_div(fee.raw, close_raw, delta.raw));
        out.open_fee = FixedE9::from_raw(detail::checked_sub(fee.raw, out.close_fee.raw));

        // A full close releases the entire basis rather than a ratio of it:
        // the pro-rata truncation would otherwise leave a residue behind in a
        // position that is flat.
        const std::int64_t released = close_raw == magnitude(open_qty)
                                          ? cost_.raw
                                          : mul_div(cost_.raw, close_raw, magnitude(open_qty));
        const std::int64_t proceeds =
            signed_value(base, side, QtyLots::from_raw(close_raw), price);
        const std::int64_t sum = detail::checked_add(released, proceeds);

        // Inverse inverts, and this is the whole trap: a long inverse position
        // is denominated in the base currency, and its base value SHRINKS as
        // the price rises. Same sum, opposite meaning.
        realized_ = FixedE9::from_raw(detail::checked_add(
            realized_.raw, base.kind == ContractKind::Inverse ? sum : -sum));
        out.realized = FixedE9::from_raw(base.kind == ContractKind::Inverse ? sum : -sum);
        cost_ = FixedE9::from_raw(detail::checked_sub(cost_.raw, released));
    }

    if (add_raw > 0) {
        cost_ = FixedE9::from_raw(detail::checked_add(
            cost_.raw, signed_value(base, side, QtyLots::from_raw(add_raw), price)));
    }

    qty_ = QtyLots::from_raw(side == Side::Buy ? detail::checked_add(open_qty, delta.raw)
                                               : detail::checked_sub(open_qty, delta.raw));
    fees_ = FixedE9::from_raw(detail::checked_add(fees_.raw, fee.raw));
    ++fill_count_;
    ts_updated_ = ts;
    out.closed = qty_.raw == 0;
    if (out.closed) {
        cost_ = FixedE9::from_raw(0);
    }
    return out;
}

FixedE9 Position::avg_entry_price(const InstrumentBase& base) const noexcept {
    if (qty_.raw == 0 || cost_.raw == 0) {
        return FixedE9::undef();
    }
    const auto abs_cost = magnitude(cost_.raw);
    if (base.kind == ContractKind::Inverse) {
        // value = units * cv / price, so price = units * cv / value.
        const auto units = detail::checked_mul(magnitude(qty_.raw), base.lot_size.raw);
        return FixedE9::from_raw(mul_div(units, base.contract_value.raw, abs_cost));
    }
    const auto quantity = base_quantity(base, QtyLots::from_raw(magnitude(qty_.raw)));
    if (quantity.is_undef() || quantity.raw == 0) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(mul_div(abs_cost, k_e9, quantity.raw));
}

FixedE9 unrealized_pnl(const InstrumentBase& base,
                       const Position& position,
                       PriceTicks mark) noexcept {
    if (position.flat()) {
        return FixedE9::from_raw(0);
    }
    const auto qty = position.qty();
    const auto value =
        settlement_value(base, mark, QtyLots::from_raw(magnitude(qty.raw)));
    if (value.is_undef()) {
        return FixedE9::undef();
    }
    const std::int64_t marked = qty.raw > 0 ? value.raw : -value.raw;
    const std::int64_t cost = position.cost().raw;
    return FixedE9::from_raw(base.kind == ContractKind::Inverse
                                 ? detail::checked_sub(cost, marked)
                                 : detail::checked_sub(marked, cost));
}

} // namespace raikiri::portfolio
