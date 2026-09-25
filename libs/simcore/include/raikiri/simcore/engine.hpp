#pragma once

// client commands come back as the event stream a live adapter would produce; background liquidity
// emits no order events

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/simcore/book.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace raikiri::simcore {

// bbo_changed lets the caller publish only when the top of book moved
struct SimResult {
    std::vector<OrderEvent> events;
    std::vector<TradeTick> trades;
    bool bbo_changed{false};
};

class Engine {
public:
    Engine() = default;

    struct ClientOrderMeta {
        InstrumentId instrument{};
        ClientOrderId client_order_id{};
        ExchangeClientOrderId exchange_client_order_id{};
        ExchangeOrderId exchange_order_id{};
        Side side{Side::Buy};
        OrderType order_type{OrderType::Limit};
        TimeInForce tif{TimeInForce::Gtc};
        OrderFlags flags{OrderFlags::None};
        PriceTicks price{};
        QtyLots qty{};    // original total
        QtyLots leaves{}; // remaining
    };

    // Snapshot of currently-resting client orders (reconciliation view).
    [[nodiscard]] std::vector<ClientOrderMeta> open_client_orders() const {
        std::vector<ClientOrderMeta> out;
        out.reserve(client_orders_.size());
        for (const auto& [id, meta] : client_orders_) {
            out.push_back(meta);
        }
        return out;
    }

    [[nodiscard]] SimResult handle(const OrderCmd& cmd, UnixNanos now);

    // Background liquidity (owner 0).
    std::uint64_t place_background_limit(
        InstrumentId instrument, Side side, PriceTicks price, QtyLots qty, UnixNanos now, SimResult& out);
    void place_background_market(InstrumentId instrument, Side side, QtyLots qty, UnixNanos now, SimResult& out);

    bool cancel_background(InstrumentId instrument, std::uint64_t sim_id, SimResult* out = nullptr);

    // the venue's positions, folded independently from the client fills it printed
    [[nodiscard]] const std::unordered_map<InstrumentId, QtyLots>& positions() const noexcept { return positions_; }

    // nullptr / nullopt when the instrument has never traded here.
    [[nodiscard]] const Book* book(InstrumentId instrument) const noexcept;
    [[nodiscard]] std::optional<Book::BboLevels> bbo(InstrumentId instrument) const;

private:
    Book& book_for(InstrumentId instrument);
    [[nodiscard]] ExecId next_exec_id() noexcept;
    [[nodiscard]] OrderEvent make_event(const ClientOrderMeta& meta, OrderEventType type, UnixNanos now) const;
    [[nodiscard]] OrderEvent make_event_from_cmd(const OrderCmd& cmd, OrderEventType type, UnixNanos now) const;
    [[nodiscard]] std::optional<std::uint64_t> resolve_target(const OrderCmd& cmd) const;
    void handle_create(const OrderCmd& cmd, UnixNanos now, SimResult& out);
    void handle_cancel(const OrderCmd& cmd, UnixNanos now, SimResult& out);
    void handle_amend(const OrderCmd& cmd, UnixNanos now, SimResult& out);
    void fold_position(InstrumentId instrument, Side side, QtyLots qty);
    void emit_fills(const ClientOrderMeta* taker,
                    QtyLots taker_leaves_start,
                    Side taker_side,
                    InstrumentId instrument,
                    const MatchResult& match,
                    UnixNanos now,
                    SimResult& out);

    std::unordered_map<InstrumentId, Book> books_;
    std::unordered_map<InstrumentId, QtyLots> positions_;
    std::unordered_map<std::uint64_t, ClientOrderMeta> client_orders_; // sim id -> meta
    std::unordered_map<ExchangeClientOrderId, std::uint64_t> by_exch_client_;
    std::uint64_t next_sim_id_{1};   // ExchangeOrderId is this, as decimal text
    std::uint64_t next_exec_seq_{1}; // ExecId "S-<seq>", unique per fill side
};

} // namespace raikiri::simcore
