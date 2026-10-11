#pragma once

// orders this gateway sent, keyed by client order index, and the events the account stream makes of them;
// fills come from trades alone, the order pushes give accepts and closes

#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/lighter/account.hpp>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::lighter {

struct TradeSide {
    std::uint64_t order_index{0};
    std::int64_t client_index{0};
    bool is_ask{false};
    bool maker{false};
};

[[nodiscard]] TradeSide side_of(const WireTrade& trade, std::int64_t account_index) noexcept;

[[nodiscard]] ExchangeOrderId order_id_of(std::uint64_t order_index) noexcept;
[[nodiscard]] ExecId trade_id_of(std::uint64_t trade_id) noexcept;

class Lifecycle {
public:
    Lifecycle(exec::GatewaySink& sink, const exec::InstrumentTable& instruments, std::int64_t account_index) noexcept
        : sink_{&sink}, instruments_{&instruments}, account_{account_index} {}

    void track(const OrderCmd& cmd);
    void forget(const ExchangeClientOrderId& id) noexcept;

    [[nodiscard]] const OrderCmd* find(std::int64_t client_index) const noexcept;

    void on_order(const WireOrder& row, UnixNanos received) noexcept;
    void on_trade(const WireTrade& trade, UnixNanos received) noexcept;

    [[nodiscard]] std::size_t tracked() const noexcept { return orders_.size(); }

private:
    struct Tracked {
        OrderCmd cmd{};
        std::uint64_t order_index{0};
        std::int64_t filled{0};
        bool accepted{false};
        // a close that arrived before its trades waits for them; -1 while none has
        std::int64_t closed_at_filled{-1};
        OrderEventType close{OrderEventType::Canceled};
    };

    [[nodiscard]] OrderEvent
    event_of(const Tracked& tracked, OrderEventType type, QtyLots leaves, UnixNanos received) const noexcept;
    void accept_once(Tracked& tracked, UnixNanos received) noexcept;
    // true when the close went out and the entry is spent
    bool try_close(Tracked& tracked, UnixNanos received) noexcept;

    exec::GatewaySink* sink_;
    const exec::InstrumentTable* instruments_;
    std::int64_t account_;
    std::unordered_map<std::int64_t, Tracked> orders_;
    std::unordered_set<std::uint64_t> seen_trades_;
};

} // namespace raikiri::venues::lighter
