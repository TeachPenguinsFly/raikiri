#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/orders.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bybit::detail {

namespace {

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the private socket, and
// nothing on either side can tell whether the other happened. This class is
// the only thing that knows both: it holds the table of orders handed over and
// not yet answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
class BybitExec final : public exec::ExecutionClient {
public:
    explicit BybitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        stream_.start();
        api_.start();
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_.connected(); }

    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    // Forwarded, not reimplemented. These are reconciliation reads and belong
    // to the REST api; ExecutionClient names them because the gateway's
    // pollers reach a venue through one handle.
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

private:
    // What a command is asking the venue to do. The three share one path
    // because they share one failure: an unanswered request.
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because the one thing this process cannot
    // know is whether the venue acted. Reconciliation asks the venue what it
    // holds and is what finally closes these.
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             in_flight_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            emit_gap(stale);
        }
    }

    void emit_gap(const exec::InFlight& stale) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::ReconGap;
        event.instrument = stale.instrument;
        event.client_order_id = stale.client_order_id;
        event.exchange_client_order_id = stale.exchange_client_order_id;
        event.side = stale.side;
        event.qty = stale.qty;
        // UNDEF, not zero: leaves of zero would read as "nothing is working",
        // and whether anything is working is exactly what is unknown here.
        event.leaves_qty = QtyLots::undef();
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            reject(cmd, to_string(request.error()));
            return;
        }
        if (const auto allowed = api_.spend(
                exec::RateBucket::OrderEntry, api_.costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            reject(cmd, allowed.error().message.view());
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!api_.send(
                *request,
                [this, client_order_id, instrument, side, qty, kind](net::RestResponse&& response) {
                    on_response(std::move(response), client_order_id, instrument, side, qty, kind);
                })) {
            reject(cmd, "rest queue full");
            return;
        }
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
        (void)in_flight_.add({.client_order_id = cmd.client_order_id,
                              .exchange_client_order_id = cmd.exchange_client_order_id,
                              .instrument = cmd.instrument,
                              .side = cmd.side,
                              .qty = cmd.qty,
                              .kind = cmd.kind,
                              .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        // Any answer at all settles it, including a refusal: what the table
        // tracks is silence, not failure.
        if (!response.transport_failed()) {
            (void)in_flight_.resolve(client_order_id);
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed — that is how this process once recorded a refusal
            // opposite a position bybit really held. The order stays in the
            // table, the desk is told the transport's own words, and the sweep
            // above reports it as a gap if no answer ever arrives.
            state_->sink->on_order_error(
                client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_envelope(response.body);
        if (!envelope.has_value()) {
            emit_reject(
                client_order_id,
                instrument,
                side,
                qty,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->ret_code == k_ret_ok) {
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->ret_code)) {
            return;
        }
        if (classify(envelope->ret_code) == exec::ErrorClass::RateLimited) {
            api_.penalize(exec::RateBucket::OrderEntry);
        }
        emit_reject(client_order_id,
                    instrument,
                    side,
                    qty,
                    kind,
                    exec::VenueError::make(
                        classify(envelope->ret_code), envelope->ret_code, envelope->ret_msg));
    }

    // The reason is not optional. Every refusal here reaches the sink with the
    // verdict that caused it, because the terminal event itself has no room for
    // one and a rejection nobody can explain costs an hour at the exchange.
    void emit_reject(ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind,
                     const exec::VenueError& error) {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    void reject(const OrderCmd& cmd, std::string_view why) noexcept {
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    cmd.kind == CmdKind::Cancel  ? Kind::Cancel
                    : cmd.kind == CmdKind::Amend ? Kind::Amend
                                                 : Kind::Create,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, why));
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    exec::InFlightTable in_flight_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BybitExec>(std::move(state));
}

} // namespace raikiri::venues::bybit::detail
