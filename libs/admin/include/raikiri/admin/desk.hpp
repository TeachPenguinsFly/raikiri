#pragma once

// writes an intent for strategyd; the command rings are single-producer and strategyd owns them

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
    GatewayGone,   // the instrument is listed but its gateway is not attached
    BadPrice,      // unparseable, or not a multiple of the tick
    BadQty,        // unparseable, or not a multiple of the lot
    PriceOnMarket, // a market order carrying a price means two things
    MissingPrice,  // a limit order without one means none
    RingFull,      // strategyd is not draining
    RingHeld,      // another process produces this gateway's order_intents
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

    // `key` makes a retry idempotent: the same key returns the same id and writes nothing
    [[nodiscard]] std::expected<ClientOrderId, DeskError>
    place(const DeskOrder& order, std::string_view key, UnixNanos now);

    // cancels only the desk's own orders; a strategy's order is refused
    [[nodiscard]] std::expected<void, DeskError> cancel(ClientOrderId id, UnixNanos now);

private:
    [[nodiscard]] ClientOrderId mint() noexcept;

    const Registry* registry_;
    std::uint64_t seq_;
    // bounded, oldest evicted; the key is client-supplied
    std::unordered_map<std::string, std::uint64_t> seen_;
    std::deque<std::string> order_;
};

} // namespace raikiri::admin
