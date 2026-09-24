#pragma once

// one execution from the venue's trade history; `qty` is the fill size and says nothing of leaves

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_events.hpp>

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
