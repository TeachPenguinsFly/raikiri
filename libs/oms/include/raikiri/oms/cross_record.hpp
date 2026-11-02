#pragma once

// two orders crossed inside the desk: the one fact a restart and every reader rebuild both legs from

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>

#include <cstdint>

namespace raikiri::oms {

struct CrossLeg {
    ClientOrderId order{};
    QtyLots leaves_after{QtyLots::from_raw(0)};
    FixedE9 fee{FixedE9::from_raw(0)};
    // the order's book's position once this leg is in it
    QtyLots net_qty{QtyLots::from_raw(0)};
    FixedE9 cost{FixedE9::from_raw(0)};
    FixedE9 realized{FixedE9::from_raw(0)};
    FixedE9 fees{FixedE9::from_raw(0)};
    std::uint32_t fill_count{0};
    UnixNanos ts_opened{};
    bool desk{false};
};

struct CrossRecord {
    InstrumentId instrument{};
    SymbolText symbol{};
    PriceTicks price{};
    QtyLots qty{QtyLots::from_raw(0)};
    // the best touch across venues the price was taken from
    FixedE9 ref_bid{};
    FixedE9 ref_ask{};
    FixedE9 notional{FixedE9::from_raw(0)};
    FixedE9 saved{FixedE9::from_raw(0)};
    UnixNanos ts{};
    CrossLeg buy{};
    CrossLeg sell{};
};

// unique to the pair: a crossing consumes one of the two, so they never cross again
[[nodiscard]] ExecId cross_exec_id(ClientOrderId buy, ClientOrderId sell, Side side) noexcept;

// the fill one leg's order takes; the same event live and in a replay
[[nodiscard]] OrderEvent cross_leg_event(const CrossRecord& record, Side side) noexcept;

} // namespace raikiri::oms
