#pragma once

// turns the backend's all-clients events into each client's ring; runs on one thread

#include <raikiri/clientapi/application/book.hpp>
#include <raikiri/clientapi/application/desk.hpp>
#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/application/ports.hpp>

#include <deque>
#include <map>
#include <string>
#include <variant>

namespace raikiri::clientapi {

class PrivateFeed {
public:
    PrivateFeed(Backend& backend, ClientBook& book, StreamHub& hub, InstrumentDirectory& instruments, WallClock wall);

    void on_order_event(OrderEventRow event);
    void on_fill(FillRow fill);
    void on_gap(std::string_view reason);

    [[nodiscard]] bool has_parked() const noexcept { return !parked_.empty(); }

    // when the next tick is due, in nanoseconds from now
    [[nodiscard]] std::uint64_t tick();

private:
    using Parked = std::variant<OrderEventRow, FillRow>;

    struct Waiting {
        Parked event{};
        std::uint64_t expires_ns{0};
    };

    struct AlgoSeen {
        std::string status{};
        std::string filled{};
        std::string working{};
        std::int64_t slices_fired{0};
    };

    [[nodiscard]] bool deliver(const OrderEventRow& event);
    [[nodiscard]] bool deliver(const FillRow& fill);
    void park(Parked event);
    void retry_parked(std::uint64_t now_ns);
    void poll_algos();
    void poll_balances();
    void poll_fills(std::uint64_t now_ns);

    Backend* backend_;
    ClientBook* book_;
    StreamHub* hub_;
    InstrumentDirectory* instruments_;
    WallClock wall_;
    std::deque<Waiting> parked_;
    std::map<std::string, AlgoSeen> algos_;
    std::map<std::string, std::vector<BalanceRow>> balances_;
    std::uint64_t fills_since_ns_{0};
    std::string fills_cursor_;
    std::uint64_t next_algos_ns_{0};
    std::uint64_t next_balances_ns_{0};
    std::uint64_t next_fills_ns_{0};
};

} // namespace raikiri::clientapi
