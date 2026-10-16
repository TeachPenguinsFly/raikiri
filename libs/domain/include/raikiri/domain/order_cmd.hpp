#pragma once

// the strategy's instruction to a gateway; its fixed 128-byte layout is the shm ring slot

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <cstdint>
#include <type_traits>

namespace raikiri {

enum class CmdKind : std::uint8_t {
    Create = 0,
    Cancel,
    Amend, // price/qty modify; venues without native amend reject via caps
};

struct OrderCmd {
    CmdKind kind{CmdKind::Create};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    OrderFlags flags{OrderFlags::None};
    std::uint8_t pad_[3]{};
    InstrumentId instrument{};
    ClientOrderId client_order_id{};
    ExchangeClientOrderId exchange_client_order_id{}; // 27 B
    std::uint8_t pad2_[5]{};
    ExchangeOrderId target_exchange_order_id{}; // cancel/amend target; 41 B
    std::uint8_t pad3_[3]{};
    // a taker's bid for an earlier place in the block, 1e-8 of filled notional; hyperliquid only, zero is none
    std::uint32_t priority_e8{0};
    PriceTicks price{}; // amend: new price
    QtyLots qty{};      // amend: new qty
    UnixNanos ts_created{};
};

static_assert(sizeof(OrderCmd) == 128);
static_assert(std::is_trivially_copyable_v<OrderCmd>);

} // namespace raikiri
