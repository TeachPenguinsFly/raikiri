#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::mexc::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// an order goes out over rest and is answered on the private socket;
// this tracks orders with no answer yet
class MexcExec final : public exec::ExecutionClient {
public:
    MexcExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_, market}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {
        if (perp_) {
            stream_ = std::make_unique<PrivateStream>(*state_);
            state_->private_client = &stream_->client();
        }
    }

    ~MexcExec() override {
        if (state_->private_client == (stream_ ? &stream_->client() : nullptr)) {
            state_->private_client = nullptr;
        }
    }

    // registered in state shared with the data client, so not copyable
    MexcExec(const MexcExec&) = delete;
    MexcExec& operator=(const MexcExec&) = delete;
    MexcExec(MexcExec&&) = delete;
    MexcExec& operator=(MexcExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (stream_) {
            stream_->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (stream_) {
            stream_->stop();
        }
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        // spot has no user stream, so spot alone is connected once rest is
        const bool stream_up = stream_ == nullptr || stream_->connected();
        return started_ && stream_up && (spot_ || perp_);
    }

    // sweep rest connections the venue closed so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // an unanswered order is reported as a gap and left for reconciliation
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
            // undefined leaves; whether anything is working is unknown
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        if (cmds.empty()) {
            return;
        }
        // spot has no cancel by id list and futures caps its array
        if (cmds.front().instrument.market() == MarketType::Spot) {
            for (const auto& cmd : cmds) {
                cancel(cmd);
            }
            return;
        }
        for (std::size_t at = 0; at < cmds.size(); at += k_max_batch_cancel) {
            const auto chunk = cmds.subspan(at, std::min(k_max_batch_cancel, cmds.size() - at));
            const auto request = api_.builder().cancel_batch(
                chunk, state_->instruments, state_->venue_now(MarketType::Perp));
            if (!request.has_value()) {
                // an order known only by our id is cancelled one call at a time
                for (const auto& cmd : chunk) {
                    cancel(cmd);
                }
                continue;
            }
            // per-order verdicts are in the body; the callback reports only transport failure
            const std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
            if (!api_.send(*request, [this, pending](net::RestResponse&& response) {
                    if (!response.transport_failed() && response.status >= 200 &&
                        response.status < 300) {
                        return;
                    }
                    // a lost connection must not mark the chunk refused; the venue
                    // may have taken every cancel
                    if (response.transport_failed()) {
                        for (const auto& cmd : pending) {
                            state_->sink->on_order_error(
                                cmd.client_order_id,
                                exec::VenueError::make(
                                    exec::ErrorClass::Connectivity,
                                    0,
                                    response.error.empty()
                                        ? std::string_view{"no response from venue"}
                                        : std::string_view{response.error}));
                        }
                        return;
                    }
                    const auto refusal =
                        rest_failure(response.body, pending.front().instrument.market());
                    for (const auto& cmd : pending) {
                        (void)unanswered_.resolve(cmd.client_order_id);
                        emit_reject(cmd, Kind::Cancel, refusal);
                    }
                })) {
                for (const auto& cmd : chunk) {
                    emit_reject(
                        cmd,
                        Kind::Cancel,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            }
        }
    }

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

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto market = cmd.instrument.market();
        const auto now = state_->venue_now(market);
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            // never left this process
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

    void
    on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind, MarketType market) {
        // any answer settles it, including a refusal
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
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (market == MarketType::Spot) {
            if (!http_ok) {
                if (kind == Kind::Cancel) {
                    if (const auto err = parse_spot_error(response.body);
                        err.has_value() && is_benign_spot_cancel_failure(err->code)) {
                        return; // the order already finished; reconciliation settles it
                    }
                }
                emit_reject(cmd, kind, rest_failure(response.body, market));
                return;
            }
            // spot has no user stream, so the create response is the ack
            if (kind == Kind::Create) {
                if (const auto wire = parse_spot_order(response.body); wire.has_value()) {
                    if (const auto event =
                            map_spot_order_event(*wire, state_->instruments, state_->clock->wall());
                        event.has_value()) {
                        state_->sink->on_order_event(*event);
                    }
                }
            }
            return;
        }

        // futures answers 200 with success:false; the envelope code decides
        const auto envelope = parse_rest_envelope(response.body);
        const bool accepted =
            http_ok && envelope.has_value() && envelope->success && envelope->code == k_ok;
        if (accepted) {
            return; // the socket's push carries the lifecycle
        }
        if (kind == Kind::Cancel && envelope.has_value() &&
            is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit_reject(cmd, kind, rest_failure(response.body, market));
    }

    // `OrderEvent` has no room for text, so the reason goes to the sink here
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
        // a refused create leaves nothing; a refused cancel or amend leaves the order unchanged
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> stream_;
    RestApi api_;
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<MexcExec>(std::move(state), market);
}

} // namespace raikiri::venues::mexc::internal
