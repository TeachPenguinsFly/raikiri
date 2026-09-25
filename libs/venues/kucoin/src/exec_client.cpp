#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/orders.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::kucoin::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

// orders go out over rest and come back on the private socket; this tracks unanswered ones and ages them out
class KucoinExec final : public exec::ExecutionClient {
public:
    KucoinExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, api_{*state_, market},
          // the bullet token expires, so every reopen fetches a new one from the api
          stream_{*state_, market, /*stream_id=*/1, [this] { return api_.private_ticket(); }} {}

    std::expected<void, exec::VenueError> start() override {
        if (!state_->credentials.complete()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kucoin: key, secret and passphrase required")};
        }
        // rest first: only a signed post on this pool mints the private socket's token
        api_.start();
        stream_.start();
        return {};
    }

    void stop() override {
        stream_.stop();
        api_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_.connected(); }

    // sweeps rest connections the venue closed, so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // kucoin's cancel-all would also cancel orders outside this batch
        for (const auto& cmd : cmds) {
            cancel(cmd);
        }
    }

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

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

private:
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
        if (!api_.serves(cmd.instrument.market())) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, "no link for this instrument's market"));
            return;
        }
        const auto now = state_->venue_now();
        auto& builder = api_.builder();
        const auto request = kind == Kind::Create   ? builder.create_order(cmd, state_->instruments, now)
                             : kind == Kind::Cancel ? builder.cancel_order(cmd, state_->instruments, now)
                                                    : builder.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto exchange_client_order_id = cmd.exchange_client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!api_.send(
                *request,
                [this, client_order_id, exchange_client_order_id, instrument, side, qty, kind](
                    net::RestResponse&& response) {
                    on_response(
                        std::move(response), client_order_id, exchange_client_order_id, instrument, side, qty, kind);
                })) {
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded before the send, since the callback may never run
        (void)unanswered_.add({.client_order_id = client_order_id,
                               .exchange_client_order_id = exchange_client_order_id,
                               .instrument = instrument,
                               .side = side,
                               .qty = qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     ExchangeClientOrderId exchange_client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        const auto refuse = [&](const exec::VenueError& error) {
            emit(client_order_id, exchange_client_order_id, instrument, side, qty, kind, error);
        };
        // any answer settles it; the table tracks silence
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(client_order_id);
        }
        if (response.transport_failed()) {
            // the request may have reached the matcher, so this is not a rejection
            state_->sink->on_order_error(client_order_id,
                                         exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                0,
                                                                response.error.empty()
                                                                    ? std::string_view{"no response from venue"}
                                                                    : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            refuse(exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            return; // the private socket carries the lifecycle from here
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code, envelope->msg)) {
            return; // the order was already gone, which is what was asked for
        }
        refuse(exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg));
    }

    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        emit(cmd.client_order_id, cmd.exchange_client_order_id, cmd.instrument, cmd.side, cmd.qty, kind, error);
    }

    // a rest refusal never reaches the private socket, so the lifecycle event is made here
    void emit(ClientOrderId client_order_id,
              ExchangeClientOrderId exchange_client_order_id,
              InstrumentId instrument,
              Side side,
              QtyLots qty,
              Kind kind,
              const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.exchange_client_order_id = exchange_client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    RestApi api_;
    PrivateStream stream_;
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state, MarketType market) {
    return std::make_unique<KucoinExec>(std::move(state), market);
}

} // namespace raikiri::venues::kucoin::internal
