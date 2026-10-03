#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/kucoin/endpoints.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/trade.hpp>

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

constexpr std::uint32_t k_trade_stream_id = 2;

// a request is never left without a verdict, even where config sets no answer timeout
constexpr std::uint64_t k_default_ws_timeout_ns = 10'000'000'000ULL;

// the session's ping interval is 18 s; a protocol ping keeps it, since the json ping wants its own id
constexpr std::uint32_t k_trade_ping_ms = 15'000;

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
        open_trade_socket();
        return {};
    }

    void stop() override {
        stream_.stop();
        if (trade_) {
            trade_->stop();
            trade_.reset();
        }
        api_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_.connected(); }

    // sweeps rest connections the venue closed, so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        if (trade_) {
            trade_->poll();
        }
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
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
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
        if (send_ws(cmd, kind, request->body)) {
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
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    void open_trade_socket() {
        if (!state_->credentials.complete()) {
            return;
        }
        net::WsRpcConfig config{};
        config.ws.host = std::string{ep::k_ws_trade_host};
        config.ws.target = std::string{ep::k_ws_trade_path};
        config.ws.ping_interval_ms = k_trade_ping_ms;
        config.ws.target_for_connect = [this] { return codec_.target(state_->venue_now()); };
        config.request_timeout_ns = state_->order_answer_timeout_ns != 0 ? state_->order_answer_timeout_ns
                                                                         : k_default_ws_timeout_ns;
        trade_ = std::make_unique<net::WsRpc>(state_->io, *state_->clock, std::move(config), codec_);
        trade_->set_observer([this](net::WsRpcState state, std::string_view) {
            if (state == net::WsRpcState::LoggingIn) {
                return;
            }
            state_->sink->on_connectivity({.state = state == net::WsRpcState::Ready ? exec::ConnState::Connected
                                                    : state == net::WsRpcState::AuthFailed
                                                        ? exec::ConnState::AuthFailed
                                                        : exec::ConnState::Disconnected,
                                           .stream_id = k_trade_stream_id,
                                           .ts = state_->clock->wall()});
        });
        trade_->start();
    }

    // false leaves the command for rest with nothing sent; amend has no classic op on the socket
    [[nodiscard]] bool send_ws(const OrderCmd& cmd, Kind kind, std::string_view body) noexcept {
        if (!trade_ || !trade_->ready() || kind == Kind::Amend) {
            return false;
        }
        const auto id = trade_->next_id();
        const auto frame = kind == Kind::Create ? ws_frames_.place(id, cmd.instrument.market(), body)
                                                : ws_frames_.cancel(id, cmd, state_->instruments);
        if (!frame.has_value()) {
            return false;
        }
        return trade_->request(id, *frame, [this, cmd, kind](const net::WsRpcReply& reply) {
            if (reply.outcome == net::WsRpcOutcome::Unknown) {
                exec::report_unknown_outcome(
                    *state_->sink, exec::in_flight_of(cmd, state_->clock->mono()), state_->clock->wall(), reply.detail);
                return;
            }
            const auto parsed = parse_trade_reply(reply.payload);
            if (!parsed.has_value()) {
                emit_reject(
                    cmd, kind, exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable trade reply"));
                return;
            }
            settle(cmd.client_order_id,
                   cmd.exchange_client_order_id,
                   cmd.instrument,
                   cmd.side,
                   cmd.qty,
                   kind,
                   parsed->code,
                   parsed->msg);
        });
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     ExchangeClientOrderId exchange_client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
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
            emit(client_order_id,
                 exchange_client_order_id,
                 instrument,
                 side,
                 qty,
                 kind,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        settle(client_order_id, exchange_client_order_id, instrument, side, qty, kind, envelope->code, envelope->msg);
    }

    // the same verdict whichever transport carried the command
    void settle(ClientOrderId client_order_id,
                ExchangeClientOrderId exchange_client_order_id,
                InstrumentId instrument,
                Side side,
                QtyLots qty,
                Kind kind,
                std::int64_t code,
                std::string_view msg) noexcept {
        if (code == k_ok) {
            return; // the private socket carries the lifecycle from here
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(code, msg)) {
            return; // the order was already gone, which is what was asked for
        }
        emit(client_order_id,
             exchange_client_order_id,
             instrument,
             side,
             qty,
             kind,
             exec::VenueError::make(classify(code), code, msg));
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
    TradeCodec codec_{state_->credentials};
    TradeFrameBuilder ws_frames_;
    std::unique_ptr<net::WsRpc> trade_;
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state, MarketType market) {
    return std::make_unique<KucoinExec>(std::move(state), market);
}

} // namespace raikiri::venues::kucoin::internal
