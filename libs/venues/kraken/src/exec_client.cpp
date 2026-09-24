#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/orders.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::kraken::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// tracks orders sent over rest until the private socket answers; emits a gap for the unanswered
class KrakenExec final : public exec::ExecutionClient {
public:
    explicit KrakenExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kraken futures has no demo environment")};
        }
        if (!state_->signer.has_credentials()) {
            // every path signs, so missing credentials fail once at start
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kraken: credentials not loaded")};
        }
        api_.start();
        stream_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return started_ && stream_.connected();
    }

    // sweeps the rest pool so an idle gateway never writes an order into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

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

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    static StatusKind status_kind(Kind kind) noexcept {
        switch (kind) {
        case Kind::Cancel:
            return StatusKind::Cancel;
        case Kind::Amend:
            return StatusKind::Edit;
        case Kind::Create:
            break;
        }
        return StatusKind::Send;
    }

    // an unanswered order is reported as a gap, not a rejection; reconciliation closes it
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // unknown, so leaves is undef rather than zero
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        auto& builder = api_.builder();
        auto request = kind == Kind::Create   ? builder.create_order(cmd, state_->instruments, now)
                       : kind == Kind::Cancel ? builder.cancel_order(cmd, state_->instruments, now)
                                              : builder.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded before the callback, which may never run
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        // any answer settles it; the table tracks silence
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(cmd.client_order_id);
        }
        if (response.transport_failed()) {
            // the request may have reached the matcher, so this is not a rejection
            state_->sink->on_order_error(
                cmd.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (response.status < 200 || response.status >= 300 || !envelope.has_value() ||
            !envelope->ok) {
            if (kind == Kind::Cancel && envelope.has_value() &&
                is_benign_cancel_failure(envelope->error)) {
                return; // the order already finished; reconciliation settles it
            }
            emit_reject(cmd, kind, rest_failure(response.body));
            return;
        }
        // the envelope says success for refused orders too; status is the verdict
        const auto verdict = parse_order_status(response.body, status_kind(kind));
        if (!verdict.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "no status in the response"));
            return;
        }
        switch (kind) {
        case Kind::Create:
            if (!is_send_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // accepted comes from the open_orders delta; binding the id here routes a fill that arrives first
            bind_ack(verdict->order_id, cmd);
            return;
        case Kind::Cancel:
            if (!is_cancel_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
            }
            return;
        case Kind::Amend:
            if (!is_edit_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // open_orders has no reason for an edit, so this response is the only confirmation
            emit_updated(verdict->order_id, cmd);
            return;
        }
    }

    void bind_ack(std::string_view order_id, const OrderCmd& cmd) {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            return;
        }
        state_->remember(order_id,
                         LiveOrder{.instrument = cmd.instrument,
                                   .client_order_id = cmd.exchange_client_order_id,
                                   .side = cmd.side,
                                   .order_type = cmd.order_type,
                                   .flags = cmd.flags,
                                   .price = cmd.price,
                                   .qty = cmd.qty,
                                   .leaves = cmd.qty});
    }

    void emit_updated(std::string_view order_id, const OrderCmd& cmd) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Updated;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        if (const auto id = ExchangeOrderId::from(order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = cmd.price;
        event.qty = cmd.qty;
        // editorder sets a new total size; the fills feed walks it down
        event.leaves_qty = cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
        if (const auto* memo = state_->recall(order_id); memo != nullptr) {
            auto updated = *memo;
            updated.price = cmd.price;
            updated.qty = cmd.qty;
            updated.leaves = cmd.qty;
            state_->remember(order_id, updated);
        }
    }

    // OrderEvent carries no text, so the sink gets the verdict here
    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        // a refused cancel or amend leaves the order as it was
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<KrakenExec>(std::move(state));
}

} // namespace raikiri::venues::kraken::internal
