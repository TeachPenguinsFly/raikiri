#pragma once

// What a venue says the account holds.
//
// Domain vocabulary rather than exec's, because three layers need the same
// words: the adapters that parse it, the shm channel that transports it, and
// the strategy process that compares it against the book. Transport must not
// depend on the adapter layer, so the type cannot live there.

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
