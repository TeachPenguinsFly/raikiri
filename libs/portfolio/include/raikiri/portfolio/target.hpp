#pragma once

// Target positions: saying what you want to hold instead of what to send.
//
// A strategy that emits orders loses state when a fill goes missing — the
// order is gone, and nothing will ever bring the intent back. A strategy that
// declares a target repairs itself: the next evaluation looks at what is
// actually held and asks for the difference, so a dropped fill, a reconnect or
// a restart all converge on their own. That is the whole reason this exists,
// and it is only possible because the position is now a fact this process
// knows.
//
// Nothing here sends anything. These are pure functions over quantities; the
// caller decides whether to submit, and the risk gate still has the last word.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <optional>

namespace raikiri::portfolio {

struct PortfolioTarget {
    InstrumentId instrument{};
    QtyLots qty{QtyLots::from_raw(0)}; // signed: + long, - short, 0 flat
};

struct Rebalance {
    Side side{Side::Buy};
    QtyLots qty{QtyLots::from_raw(0)}; // magnitude; zero means nothing to do
};

// What to send to reach `target`, given what is held and what is already
// working.
//
// Working orders on BOTH sides are netted off here — unlike the risk gate,
// which must assume same-side working orders fill and never assumes
// opposite-side ones cancel. The difference is deliberate: this function asks
// "what is still missing?", where an order already on the wire genuinely is
// progress toward the target; the gate asks "how bad could this get?", where
// it is not. Netting here without the gate's worst case would let a rebalance
// walk past a limit; the gate's worst case without netting here would make it
// re-send the same order every evaluation.
[[nodiscard]] Rebalance delta_to_target(QtyLots target,
                                        QtyLots position,
                                        QtyLots working_buy,
                                        QtyLots working_sell) noexcept;

// Quantity to hold so that `stop_distance` moving against the position costs
// `risk_fraction` of `equity`.
//
//   qty = equity * risk_fraction / value_of_one_lot_over(stop_distance)
//
// Returns nullopt rather than a number when the inputs make it meaningless: a
// zero or negative stop, a non-positive equity, or an instrument whose lot is
// worth nothing over that distance. A sized position derived from a stop that
// does not exist is a position sized by accident.
//
// The size is truncated, never rounded up, and the e9 arithmetic under it
// leans the same way: a size rounded up overspends the risk budget on every
// position it ever sizes, while a size one lot short costs nothing.
//
// `equity` and the answer are in the instrument's settlement currency, so on
// an inverse contract both are the base coin — mixing in a quote-denominated
// account balance there is wrong, and the caller has to convert first.
[[nodiscard]] std::optional<QtyLots> size_for_risk(const InstrumentBase& base,
                                                   FixedE9 equity,
                                                   FixedE9 risk_fraction,
                                                   PriceTicks entry,
                                                   PriceTicks stop) noexcept;

} // namespace raikiri::portfolio
