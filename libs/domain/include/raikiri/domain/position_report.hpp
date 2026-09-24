#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>

namespace raikiri {

struct PositionReport {
    InstrumentId instrument{};
    QtyLots net_qty{}; // signed: positive long, negative short
    PriceTicks avg_entry{};
    UnixNanos ts_venue{};
};

} // namespace raikiri
