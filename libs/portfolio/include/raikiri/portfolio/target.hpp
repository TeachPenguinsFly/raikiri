#pragma once

// pure functions from a target position to the order that reaches it

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

// nets working orders on both sides, unlike the risk gate
[[nodiscard]] Rebalance
delta_to_target(QtyLots target, QtyLots position, QtyLots working_buy, QtyLots working_sell) noexcept;

// size where `stop_distance` against the position costs `risk_fraction` of `equity`
// truncated, and in the instrument's settlement currency
[[nodiscard]] std::optional<QtyLots> size_for_risk(
    const InstrumentBase& base, FixedE9 equity, FixedE9 risk_fraction, PriceTicks entry, PriceTicks stop) noexcept;

} // namespace raikiri::portfolio
