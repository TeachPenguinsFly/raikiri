#pragma once

// Whose order a venue event is about, and what it moved.
//
// A VENUE KNOWS AN ORDER ONLY BY THE ID WE SENT IT. Every event off a private
// stream therefore carries `client_order_id` zero and names the order by the
// exchange id instead. Everything downstream — the position book's
// attribution, the ledger's strategy column, the fill's dedup key, the
// round-trip clock — reads the strategy out of our own id, and left at zero
// they all quietly answer "strategy 0". This restores it before anything else
// looks.
//
// AND THE LEAVES ARE READ BEFORE THE EVENT IS APPLIED, because a fill's
// quantity is the DROP in leaves and never the event's own qty: `OrderEvent
// ::qty` is documented as "order qty or fill qty", and a venue that resends a
// fill notification must derive zero and move no money.
//
// It lives here rather than in the process that loops, because getting either
// half wrong is a position error and a position error deserves a test that
// needs no gateway, no socket and no shared memory to run.

#include <raikiri/domain/order_events.hpp>
#include <raikiri/oms/order_store.hpp>

namespace raikiri::oms {

// What the store knew about an order before this event moved it.
struct Attribution {
    // Null when this process never held the order. A fill against one is money
    // that moved on an account we hold and nobody here can explain — the
    // caller is expected to say so out loud, not to skip it.
    const Order* order{nullptr};
    // UNDEF, not zero, when the order is unknown: this process does not know
    // what that order had left, and a zero would be a claim that it had
    // nothing. Read it only after checking `order`.
    QtyLots prev_leaves{};
};

// Resolves the event's order by EITHER id and restores ours onto it.
//
// The event is taken by reference and MUTATED: the caller works on a copy of
// the record, because the id has to be on the event before the store, the
// book and the ledger each read it.
[[nodiscard]] Attribution attribute(OrderEvent& event, const OrderStore& store) noexcept;

// What the fill moved, as the drop in leaves. Zero — never negative — when the
// order is unknown or the venue resent an event the book already took.
[[nodiscard]] QtyLots filled_delta(const Attribution& who, const OrderEvent& event) noexcept;

} // namespace raikiri::oms
