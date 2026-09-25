#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/orders.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::gate::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// holds orders sent over rest and not yet answered on the private socket
class GateExec final : public exec::ExecutionClient {
public:
    // gate names spot and perp pairs identically and the mappers take no market, so run one gateway per market
    GateExec(std::shared_ptr<VenueState> state, MarketType market) : state_{std::move(state)}, api_{*state_} {
        // the two private streams use different hosts and shapes; only this market's opens
        if (market == MarketType::Perp) {
            perp_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
        } else if (market == MarketType::Spot) {
            spot_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
        }
    }

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (perp_ && state_->futures_user_id == 0) {
            // fetched before the socket opens: a subscribe without the user id gets an empty stream
            if (const auto user = api_.futures_user_id(); user.has_value()) {
                state_->futures_user_id = *user;
            }
            if (state_->futures_user_id == 0) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "gate: futures account id unavailable")};
            }
        }
        if (perp_) {
            perp_->start();
        }
        if (spot_) {
            spot_->start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_) {
            perp_->stop();
        }
        if (spot_) {
            spot_->stop();
        }
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_ == nullptr || perp_->connected();
        const bool spot_up = spot_ == nullptr || spot_->connected();
        return started_ && perp_up && spot_up && (perp_ != nullptr || spot_ != nullptr);
    }

    // sweeps rest connections the venue closed, so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // gate refuses a whole batch over 20, so the span is chunked
        for (std::size_t at = 0; at < cmds.size(); at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            const auto request = api_.builder().cancel_batch(chunk, state_->instruments, state_->venue_now());
            if (!request.has_value()) {
                const auto refusal = build_failure(request.error());
                for (const auto& cmd : chunk) {
                    emit_reject(cmd, Kind::Cancel, refusal);
                }
                continue;
            }
            // per-order verdicts are in the body; this reports only transport failure
            std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
            if (!api_.send(*request, [this, pending](net::RestResponse&& response) {
                    if (!response.transport_failed() && response.status >= 200 && response.status < 300) {
                        return;
                    }
                    // a lost connection must not mark every cancel in the chunk refused
                    if (response.transport_failed()) {
                        for (const auto& cmd : pending) {
                            state_->sink->on_order_error(
                                cmd.client_order_id,
                                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                       0,
                                                       response.error.empty()
                                                           ? std::string_view{"no response from venue"}
                                                           : std::string_view{response.error}));
                        }
                        return;
                    }
                    const auto refusal = rest_failure(response.body);
                    for (const auto& cmd : pending) {
                        (void)unanswered_.resolve(cmd.client_order_id);
                        emit_reject(cmd, Kind::Cancel, refusal);
                    }
                })) {
                for (const auto& cmd : chunk) {
                    emit_reject(
                        cmd, Kind::Cancel, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            }
        }
    }

    // forwarded to the rest api; the gateway's pollers reach a venue through one handle
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

private:
    // the three share one failure path: an unanswered request
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // an unanswered order is reported as a gap; reconciliation closes it
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // undef: whether anything is working is unknown
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        auto& builder = api_.builder();
        const auto request = kind == Kind::Create   ? builder.create_order(cmd, state_->instruments, now)
                             : kind == Kind::Cancel ? builder.cancel_order(cmd, state_->instruments, now)
                                                    : builder.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        const auto market = cmd.instrument.market();
        if (!api_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            // never left this process, so the refusal is certain
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded here since the callback may never run
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind, MarketType market) {
        // any answer settles it; the table tracks silence
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(cmd.client_order_id);
        }
        if (response.transport_failed()) {
            // the request may have reached the matcher, so this is no rejection
            state_->sink->on_order_error(cmd.client_order_id,
                                         exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                0,
                                                                response.error.empty()
                                                                    ? std::string_view{"no response from venue"}
                                                                    : std::string_view{response.error}));
            return;
        }
        if (response.status >= 200 && response.status < 300) {
            // the success body is the order; caching it seeds leaves before the _new push
            if (const auto wire = parse_rest_order(response.body, market); wire.has_value()) {
                state_->remember(*wire);
            }
            return;
        }
        if (kind == Kind::Cancel) {
            if (const auto err = parse_rest_error(response.body);
                err.has_value() && is_benign_cancel_failure(err->label)) {
                return; // the order already finished; reconciliation settles it
            }
        }
        emit_reject(cmd, kind, rest_failure(response.body));
    }

    // OrderEvent has no room for text, so the sink gets the error
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
        // a failed create leaves nothing; a refused cancel or amend leaves the order as it was
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> perp_;
    std::unique_ptr<PrivateStream> spot_;
    RestApi api_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state, MarketType market) {
    return std::make_unique<GateExec>(std::move(state), market);
}

} // namespace raikiri::venues::gate::internal
