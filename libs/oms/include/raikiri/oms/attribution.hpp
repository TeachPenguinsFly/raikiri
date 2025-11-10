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

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

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

// Resolves the event's order by ANY of the three ids a venue may name it with
// — ours, the one we minted and sent, or the venue's own — and restores ours
// onto it. The third is what a fill pulled from a trade history arrives with,
// and sometimes all it arrives with.
//
// The event is taken by reference and MUTATED: the caller works on a copy of
// the record, because the id has to be on the event before the store, the
// book and the ledger each read it.
[[nodiscard]] Attribution attribute(OrderEvent& event, const OrderStore& store) noexcept;

// What the fill moved, as the drop in leaves. Zero — never negative — when the
// order is unknown or the venue resent an event the book already took.
//
// A fill whose leaves are UNDEF states its own size in `qty` instead, capped by
// what the order had left: that is a fill PULLED from the venue's trade history
// rather than pushed down its socket, and a pull reports the execution without
// reporting the order.
[[nodiscard]] QtyLots filled_delta(const Attribution& who, const OrderEvent& event) noexcept;

// Executions already taken, so the same one is never taken twice.
//
// WHY A PULL NEEDS THIS AND A SOCKET DOES NOT. A socket reports the order's
// remaining quantity, so a resent notification derives a zero drop and moves
// nothing — the arithmetic dedupes itself. A pull reports the execution, and
// the venue's trade history re-offers the same execution on every pass over the
// same window. Nothing in the numbers tells the second offer from the first.
//
// AN EXECUTION WITH NO ID IS ALWAYS ADMITTED. Some venues mint none, and
// treating every empty id as one execution would collapse a whole venue's fills
// into the first — which is worse than booking one twice, because it is silent
// and permanent. The caller gets them all and the leaves cap is what protects
// it there.
class SeenFills {
public:
    // True when this execution had not been seen, and records it. False when it
    // had — the caller must not apply it again.
    [[nodiscard]] bool admit(std::string_view exec_id, UnixNanos now);

    // Forgets what no pull window can reach back over. Called on the same timer
    // that prunes the fill deltas, and for the same reason: an unbounded set is
    // a leak with a venue-supplied key.
    void prune(UnixNanos now, std::uint64_t window_ns);

    [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

private:
    std::unordered_map<std::string, std::uint64_t> seen_;
};

} // namespace raikiri::oms
