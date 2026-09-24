#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::bitfinex::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// tracks orders sent over rest and not yet answered on the socket
// rest is the write transport because its notification carries the verdict
class BitfinexExec final : public exec::ExecutionClient {
public:
    BitfinexExec(std::shared_ptr<VenueState> state, MarketType market);

    ~BitfinexExec() override { state_->private_poll = nullptr; }

    // registers in state shared with the data client
    BitfinexExec(const BitfinexExec&) = delete;
    BitfinexExec& operator=(const BitfinexExec&) = delete;
    BitfinexExec(BitfinexExec&&) = delete;
    BitfinexExec& operator=(BitfinexExec&&) = delete;

    std::expected<void, exec::VenueError> start() override;

    void stop() override;

    [[nodiscard]] bool connected() const noexcept override;

    // sweeps dead rest connections so an idle gateway does not send into a closed socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    // unanswered orders are reported as gaps; reconciliation closes them
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

    void fire(const OrderCmd& cmd, Kind kind) noexcept;

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind);

    // every refusal carries the venue's verdict to the sink
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
    std::unique_ptr<PrivateStream> stream_;
    RestApi api_;
    exec::InFlightTable unanswered_{k_max_unanswered};
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
};

BitfinexExec::BitfinexExec(std::shared_ptr<VenueState> state, MarketType market)
    : state_{std::move(state)}, api_{*state_}, spot_{market == MarketType::Spot},
      perp_{market == MarketType::Perp} {}

std::expected<void, exec::VenueError> BitfinexExec::start() {
    api_.start();
    // without credentials the socket would be refused, so only rest runs
    if (!state_->api_key.empty() && !state_->api_secret.empty()) {
        stream_ = std::make_unique<PrivateStream>(*state_);
        stream_->start();
    }
    // the private socket's ping rides the data client's reactor
    state_->private_poll = [this](UnixNanos now) {
        if (stream_ != nullptr) {
            stream_->heartbeat(now);
        }
    };
    started_ = true;
    return {};
}

void BitfinexExec::stop() {
    state_->private_poll = nullptr;
    if (stream_) {
        stream_->stop();
    }
    api_.stop();
    started_ = false;
}

bool BitfinexExec::connected() const noexcept {
    return started_ && stream_ != nullptr && stream_->connected() && (spot_ || perp_);
}

void BitfinexExec::fire(const OrderCmd& cmd, Kind kind) noexcept {
    const auto now = state_->clock->wall();
    const auto request =
        kind == Kind::Create   ? api_.builder().create_order(cmd, state_->instruments, now)
        : kind == Kind::Cancel ? api_.builder().cancel_order(cmd, state_->instruments, now)
                               : api_.builder().amend_order(cmd, state_->instruments, now);
    if (!request.has_value()) {
        emit_reject(cmd, kind, build_failure(request.error()));
        return;
    }
    if (kind == Kind::Create) {
        // recorded first: an on push can beat the rest answer
        if (const auto cid = cid_from(cmd.exchange_client_order_id); cid.has_value()) {
            state_->remember_cid(*cid, cmd.exchange_client_order_id);
        }
    }
    if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
            on_response(std::move(response), cmd, kind);
        })) {
        // never sent, so nothing is in doubt
        emit_reject(
            cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        return;
    }
    // recorded before the callback that may never run
    (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                           .exchange_client_order_id = cmd.exchange_client_order_id,
                           .instrument = cmd.instrument,
                           .side = cmd.side,
                           .qty = cmd.qty,
                           .kind = cmd.kind,
                           .sent = state_->clock->mono()});
}

void BitfinexExec::on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
    // any answer settles it; the table tracks silence
    if (!response.transport_failed()) {
        (void)unanswered_.resolve(cmd.client_order_id);
    }
    if (response.transport_failed()) {
        // the request may have reached the matcher, so report an error only
        state_->sink->on_order_error(
            cmd.client_order_id,
            exec::VenueError::make(exec::ErrorClass::Connectivity,
                                   0,
                                   response.error.empty()
                                       ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error}));
        return;
    }
    // ["error",code,text] is the other refusal shape
    if (const auto refusal = rest_error(response.body); refusal.has_value()) {
        emit_reject(cmd, kind, *refusal);
        return;
    }
    // bitfinex answers http 200 to a refused order; the notification status decides
    const auto notification = parse_notification(response.body);
    if (!notification.has_value()) {
        emit_reject(
            cmd,
            kind,
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(notification.error())));
        return;
    }
    if (!notification_ok(*notification)) {
        emit_reject(cmd,
                    kind,
                    exec::VenueError::make(
                        classify(notification->code), notification->code, notification->text));
        return;
    }
    if (kind == Kind::Create && notification->has_order) {
        // seeding the memo lets a fill that beats the on push state its leaves
        state_->remember(notification->order.id,
                         {.client_order_id = cmd.exchange_client_order_id, .leaves = cmd.qty});
    }
    // the socket push carries the lifecycle
}

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<BitfinexExec>(std::move(state), market);
}

} // namespace raikiri::venues::bitfinex::internal
