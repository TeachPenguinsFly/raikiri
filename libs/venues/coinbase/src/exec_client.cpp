#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/orders.hpp>
#include <raikiri/venues/coinbase/rest.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"

namespace raikiri::venues::coinbase::internal {

namespace {

// a cancel naming an order coinbase already finished
constexpr int k_http_not_found = 404;

inline constexpr std::size_t k_max_unanswered = 512;

// rest order entry; no private socket, so executions are not observed live (see fill_reports)
class CoinbaseExec final : public exec::ExecutionClient {
public:
    explicit CoinbaseExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials() || state_->passphrase.empty()) {
            // AuthFailure points at the config; the passphrase is the credential most often unset
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "coinbase: key, secret and passphrase required")};
        }
        api_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        api_.stop();
        started_ = false;
    }

    // true once start() passed the credential check and the rest pool is up
    [[nodiscard]] bool connected() const noexcept override { return started_; }

    // sweeps connections the venue closed so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // unanswered orders are reported as gaps without a status change; reconciliation closes them
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
            // undef: whether anything is working is unknown
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
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

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                                   : api_.builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (kind == Kind::Create) {
            // remembered before the send: the response can arrive in the same poll
            state_->remember(cmd.exchange_client_order_id);
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            // never sent, so nothing is in doubt
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded before the callback because the callback may never run
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
            // a transport failure is not a rejection: the order may have reached the matcher
            state_->sink->on_order_error(
                cmd.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            // 404 on cancel means the order already finished; not escalated
            if (kind == Kind::Cancel && response.status == k_http_not_found) {
                return;
            }
            emit_reject(cmd, kind, http_failure(response));
            return;
        }
        switch (kind) {
        case Kind::Create: {
            // POST /orders returns the whole order, so acceptance and immediate rejection are both read here
            const auto order = parse_order(response.body);
            if (!order.has_value()) {
                emit_reject(
                    cmd,
                    kind,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order row"));
                return;
            }
            if (const auto event = map_order_event(*order,
                                                   state_->instruments,
                                                   cmd.exchange_client_order_id,
                                                   state_->clock->wall());
                event.has_value()) {
                state_->sink->on_order_event(*event);
            }
            return;
        }
        case Kind::Cancel:
            // the body is just the id; the status line is the verdict
            emit_cancelled(cmd);
            return;
        case Kind::Amend:
            return; // unreachable: amend_order never builds a request
        }
    }

    void emit_cancelled(const OrderCmd& cmd) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Canceled;
        event.side = cmd.side;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.exchange_order_id = cmd.target_exchange_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        // nothing rests after a cancel; filled quantity comes from Filled events
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // OrderEvent has no text, so the reason goes to the sink separately
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
        // a refused create leaves nothing; a refused cancel or amend leaves the order as it was
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    RestApi api_;
    bool started_{false};
    // orders handed to the transport and not yet answered
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<CoinbaseExec>(std::move(state));
}

} // namespace raikiri::venues::coinbase::internal
