#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/trade.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::okx::internal {

namespace {

inline constexpr std::size_t k_max_unanswered = 512;

constexpr std::uint32_t k_trade_stream_id = 2;

// a request is never left without a verdict, even where config sets no answer timeout
constexpr std::uint64_t k_default_ws_timeout_ns = 10'000'000'000ULL;

// okx drops a socket silent for 30 s
constexpr std::uint64_t k_trade_ping_interval_ns = 15'000'000'000ULL;

// orders go out on the trade socket, or over rest while it is down, and come back on the private socket
class OkxExec final : public exec::ExecutionClient {
public:
    explicit OkxExec(std::shared_ptr<VenueState> state) : state_{std::move(state)}, stream_{*state_}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        if (!state_->credentialed() || state_->pulls_only) {
            return {};
        }
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

    void submit(const OrderCmd& cmd) noexcept override {
        if (cmd.instrument.market() == MarketType::Option) {
            emit(cmd,
                 OrderEventType::Rejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "okx options are market data only"));
            return;
        }
        fire(api_.builder().create_order(cmd, state_->instruments, state_->venue_now()), cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        fire(api_.builder().cancel_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        fire(api_.builder().amend_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::ModifyRejected);
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto batch = api_.builder().cancel_batch(
                cmds.subspan(sent), state_->instruments, state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                break;
            }
            const auto chunk = cmds.subspan(sent, batch->consumed);
            if (!send_batch(batch->request, chunk)) {
                break;
            }
            sent += batch->consumed;
        }
        // anything the builder would not take is refused explicitly
        for (const auto& cmd : cmds.subspan(sent)) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "batch would not take this cancel"));
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

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

private:
    // sweeps idle rest connections the venue already closed
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        if (trade_) {
            trade_->poll();
        }
        sweep_unanswered();
    }

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

    void open_trade_socket() {
        net::WsRpcConfig config{};
        config.ws.host = std::string{stream_host(state_->testnet)};
        config.ws.port = std::string{stream_port()};
        config.ws.target = std::string{private_stream_path()};
        config.request_timeout_ns = state_->order_answer_timeout_ns != 0 ? state_->order_answer_timeout_ns
                                                                         : k_default_ws_timeout_ns;
        config.ping_interval_ns = k_trade_ping_interval_ns;
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

    // false leaves the command for rest with nothing sent
    [[nodiscard]] bool send_ws(const OrderCmd& cmd, OrderEventType on_failure, std::string_view body) noexcept {
        if (!trade_ || !trade_->ready()) {
            return false;
        }
        const auto code = state_->inst_id_codes.find(cmd.instrument);
        if (code == state_->inst_id_codes.end()) {
            return false;
        }
        const auto op = on_failure == OrderEventType::Rejected         ? k_op_order
                        : on_failure == OrderEventType::CancelRejected ? k_op_cancel
                                                                       : k_op_amend;
        const auto id = trade_->next_id();
        const auto frame = ws_frames_.request(id, op, body, code->second);
        if (!frame.has_value()) {
            return false;
        }
        return trade_->request(id, *frame, [this, cmd, on_failure](const net::WsRpcReply& reply) {
            if (reply.outcome == net::WsRpcOutcome::Unknown) {
                exec::report_unknown_outcome(
                    *state_->sink, exec::in_flight_of(cmd, state_->clock->mono()), state_->clock->wall(), reply.detail);
                return;
            }
            if (const auto refusal = verdict_of(reply.payload, cmd.exchange_client_order_id, on_failure);
                refusal.has_value()) {
                emit(cmd, on_failure, *refusal);
            }
        });
    }

    void fire(const std::expected<net::RestRequest, BuildError>& request,
              const OrderCmd& cmd,
              OrderEventType on_failure) noexcept {
        if (!request.has_value()) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        if (send_ws(cmd, on_failure, request->body)) {
            return;
        }
        const auto captured = cmd;
        const auto sent = api_.send(*request, [this, captured, on_failure](net::RestResponse&& r) {
            const auto refusal = verdict(r, captured.exchange_client_order_id, on_failure);
            // the request may have reached the matcher, so it stays for the sweep
            if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                state_->sink->on_order_error(captured.client_order_id, *refusal);
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            if (refusal.has_value()) {
                emit(captured, on_failure, *refusal);
            }
        });
        if (!sent) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    bool send_batch(const net::RestRequest& request, std::span<const OrderCmd> chunk) noexcept {
        std::vector<OrderCmd> captured{chunk.begin(), chunk.end()};
        const bool sent = api_.send(request, [this, captured](net::RestResponse&& r) {
            for (const auto& cmd : captured) {
                const auto refusal = verdict(r, cmd.exchange_client_order_id, OrderEventType::CancelRejected);
                // the venue may have taken every cancel in the chunk
                if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                    state_->sink->on_order_error(cmd.client_order_id, *refusal);
                    continue;
                }
                (void)unanswered_.resolve(cmd.client_order_id);
                if (refusal.has_value()) {
                    emit(cmd, OrderEventType::CancelRejected, *refusal);
                }
            }
        });
        if (!sent) {
            return false;
        }
        for (const auto& cmd : chunk) {
            (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
        }
        return true;
    }

    // a refused order answers envelope code 1 with the reason in data[].sCode; absent means accepted
    [[nodiscard]] std::optional<exec::VenueError>
    verdict(const net::RestResponse& response, const ExchangeClientOrderId& id, OrderEventType on_failure) const {
        if (response.transport_failed()) {
            return exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response from venue");
        }
        return verdict_of(response.body, id, on_failure);
    }

    // the socket's reply carries the rest envelope, so both transports reach the same verdict
    [[nodiscard]] static std::optional<exec::VenueError>
    verdict_of(std::string_view body, const ExchangeClientOrderId& id, OrderEventType on_failure) {
        const auto envelope = parse_rest_envelope(body);
        if (!envelope.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope");
        }
        static thread_local const auto storage_owner = std::make_unique<AckBuffer>();
        auto& storage = *storage_owner;
        const auto acks = parse_order_acks(body, storage);
        std::int64_t code = envelope->code;
        std::string_view text = envelope->msg;
        if (acks.has_value()) {
            for (const auto& ack : *acks) {
                // A batch answers for several orders at once, so the entry
                // that matters is the one carrying this order's id.
                if (ack.cl_ord_id == id.view() || acks->size() == 1) {
                    code = ack.s_code;
                    text = ack.s_msg;
                    break;
                }
            }
        }
        if (code == k_ok) {
            return std::nullopt; // the private stream reports what follows
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(code)) {
            return std::nullopt; // the order already filled or was gone
        }
        if (on_failure == OrderEventType::ModifyRejected && is_benign_amend_failure(code)) {
            return std::nullopt;
        }
        return exec::VenueError::make(classify(code), code, text);
    }

    void emit(const OrderCmd& cmd, OrderEventType type, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateStream stream_;
    RestApi api_;
    TradeCodec codec_{state_->signer, state_->passphrase.view(), state_->clock_offset_ns};
    TradeFrameBuilder ws_frames_;
    std::unique_ptr<net::WsRpc> trade_;
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OkxExec>(std::move(state));
}

} // namespace raikiri::venues::okx::internal
