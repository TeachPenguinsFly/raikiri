#pragma once

// One execution, as the VENUE'S TRADE HISTORY states it.
//
// Domain vocabulary rather than exec's, for the same reason PositionReport is:
// the adapters parse it, an shm channel transports it, and the strategy process
// applies it, and transport must not depend on the adapter layer.
//
// WHAT IT DOES NOT CARRY IS THE POINT. An order event off a private socket
// states the ORDER's remaining quantity, and the drop in that is the fill. This
// states the EXECUTION and says nothing about the order it belonged to — so a
// reader must take `qty` as the size and may not infer leaves from it. See
// oms::filled_delta, which is where the two shapes meet.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

namespace raikiri {

struct FillReport {
    ExecId exec_id{};
    ExchangeOrderId exchange_order_id{};
    ExchangeClientOrderId exchange_client_order_id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    PriceTicks price{};
    QtyLots qty{};
    FixedE9 fee{};
    Liquidity liquidity{Liquidity::Unknown};
    UnixNanos ts_venue{};
};

} // namespace raikiri
