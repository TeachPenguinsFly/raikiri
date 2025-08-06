#pragma once

// The manual desk: the one place a person's order becomes a command.
//
// It does NOT write the command rings. Those are single-producer and strategyd
// owns them; a second writer would be a data race, and the losslessness the
// ring is chosen for rests on there being exactly one. This writes an INTENT
// instead, which strategyd picks up and runs through the same risk gate, halt,
// order store and journal a strategy's order goes through. The indirection is
// the point: an order placed by hand that skipped any of those would be an
// order the system could not reconcile, freeze or replay.

#include <raikiri/admin/registry.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <cstdint>
#include <deque>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

namespace raikiri::admin {

enum class DeskError : std::uint8_t {
    NoSuchInstrument,
    NoSuchOrder,
    GatewayGone,  // the instrument is listed but its gateway is not attached
    BadPrice,     // unparseable, or not a multiple of the tick
    BadQty,       // unparseable, or not a multiple of the lot
    PriceOnMarket,// a market order carrying a price means two things
    MissingPrice, // a limit order without one means none
    RingFull,     // strategyd is not draining
};

[[nodiscard]] std::string_view describe(DeskError error) noexcept;

struct DeskOrder {
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    OrderFlags flags{OrderFlags::None};
    std::string price; // decimal, empty for market
    std::string qty;   // decimal
};

class ManualDesk {
public:
    explicit ManualDesk(const Registry& registry) noexcept;

    // Mints an id, writes the intent, and answers with the id the caller will
    // see the order under. `key` makes a retry idempotent: the same key
    // returns the same id and writes nothing a second time.
    [[nodiscard]] std::expected<ClientOrderId, DeskError> place(const DeskOrder& order,
                                                                std::string_view key,
                                                                UnixNanos now);

    // Queues a cancel. The desk only cancels ITS OWN orders — an id belonging
    // to a strategy is refused here, because a person reaching into a
    // strategy's book leaves that strategy holding an order it still believes
    // is working.
    [[nodiscard]] std::expected<void, DeskError> cancel(ClientOrderId id, UnixNanos now);

private:
    [[nodiscard]] ClientOrderId mint() noexcept;

    const Registry* registry_;
    std::uint64_t seq_;
    // Bounded, oldest evicted. An unbounded map is a memory leak with a
    // client-supplied key, which is the shape of a denial of service.
    std::unordered_map<std::string, std::uint64_t> seen_;
    std::deque<std::string> order_;
};

} // namespace raikiri::admin
