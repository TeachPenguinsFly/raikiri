#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/ws_api.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::binance::internal {

namespace {

// the key lapses at 60 minutes; 30 leaves room for one missed call
constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

inline constexpr std::size_t k_max_unanswered = 512;

constexpr std::uint32_t k_trade_stream_id = 2;

// a request is never left without a verdict, even where config sets no answer timeout
constexpr std::uint64_t k_default_ws_timeout_ns = 10'000'000'000ULL;

[[nodiscard]] exec::VenueError spot_refused() {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "binance spot is market data only");
}

// holds orders sent over rest and not yet answered on the user stream, and owns the listen key
class BinanceExec final : public exec::ExecutionClient {
public:
    explicit BinanceExec(std::shared_ptr<VenueState> state) : state_{std::move(state)}, api_{*state_} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    // the hook lives in state shared with the data client, which may outlive this
    ~BinanceExec() override { state_->tick_hook = nullptr; }

    BinanceExec(const BinanceExec&) = delete;
    BinanceExec& operator=(const BinanceExec&) = delete;
    BinanceExec(BinanceExec&&) = delete;
    BinanceExec& operator=(BinanceExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        started_ = true;
        if (!state_->signer.has_credentials() || state_->spot()) {
            return {};
        }
        open_trade_socket();
        return open_user_stream();
    }

    void stop() override {
        if (!started_) {
            return; // teardown calls stop twice
        }
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
        if (trade_) {
            trade_->stop();
            trade_.reset();
        }
        if (state_->signer.has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = api_.builder().listen_key_close(); request.has_value()) {
                (void)api_.pull(*request, 2'000);
            }
        }
        state_->listen_key.clear();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_ != nullptr && stream_->connected(); }

    // sweeps rest connections the venue closed, so an idle gateway does not write into a dead socket
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

    void submit(const OrderCmd& cmd) noexcept override {
        if (state_->spot()) {
            emit(cmd, OrderEventType::Rejected, spot_refused());
            return;
        }
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::Rejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        if (const auto allowed = api_.spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        const auto request = api_.builder().create_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected, build_failed(request.error()));
            return;
        }
        if (send_ws(k_ws_order_place, cmd, OrderEventType::Rejected)) {
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        if (state_->spot()) {
            emit(cmd, OrderEventType::CancelRejected, spot_refused());
            return;
        }
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        if (const auto allowed = api_.spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request = api_.builder().cancel_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, build_failed(request.error()));
            return;
        }
        if (send_ws(k_ws_order_cancel, cmd, OrderEventType::CancelRejected)) {
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        if (state_->spot()) {
            emit(cmd, OrderEventType::ModifyRejected, spot_refused());
            return;
        }
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::ModifyRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        const auto request = api_.builder().amend_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        if (send_ws(k_ws_order_modify, cmd, OrderEventType::ModifyRejected)) {
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
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

    // driven by the data client's poll: the key lapses whether or not an order is working
    void tick(UnixNanos now) noexcept {
        if (!started_ || !state_->signer.has_credentials()) {
            return;
        }
        if (state_->listen_key_expired) {
            state_->listen_key_expired = false;
            (void)open_user_stream();
            return;
        }
        if (now.ns < state_->listen_key_due_ns || state_->listen_key.empty()) {
            return;
        }
        state_->listen_key_due_ns = now.ns + k_listen_key_interval_ns;
        if (const auto request = api_.builder().listen_key_keepalive(); request.has_value()) {
            (void)api_.send(*request, [](net::RestResponse&&) {});
        }
    }

private:
    void open_trade_socket() {
        net::WsRpcConfig config{};
        config.ws.host = std::string{ep::ws_api_host(state_->testnet)};
        config.ws.target = std::string{ep::k_ws_api_path};
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

    // false leaves the command for rest with nothing sent; the rest builder's query is what gets re-signed
    [[nodiscard]] bool send_ws(std::string_view method, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        if (!trade_ || !trade_->ready()) {
            return false;
        }
        const auto id = trade_->next_id();
        const auto frame = ws_builder_.request(id, method, api_.builder().last_sign_message());
        if (!frame.has_value()) {
            return false;
        }
        return trade_->request(
            id, *frame, [this, cmd, on_failure](const net::WsRpcReply& reply) { on_ws_reply(reply, cmd, on_failure); });
    }

    void on_ws_reply(const net::WsRpcReply& reply, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        if (reply.outcome == net::WsRpcOutcome::Unknown) {
            exec::report_unknown_outcome(
                *state_->sink, exec::in_flight_of(cmd, state_->clock->mono()), state_->clock->wall(), reply.detail);
            return;
        }
        const auto parsed = parse_ws_api_reply(reply.payload);
        if (!parsed.has_value()) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable ws api reply"));
            return;
        }
        if (parsed->status != 200 && parsed->error.code == k_ok) {
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "ws api refused without a code"));
            return;
        }
        settle(cmd, on_failure, parsed->error);
    }

    // the same verdict whichever transport carried the command
    void settle(const OrderCmd& cmd, OrderEventType on_failure, const RestError& err) noexcept {
        if (err.code == k_ok) {
            return; // accepted; the user stream reports what happens next
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(err.code)) {
            return; // already filled or gone
        }
        if (classify(err.code) == exec::ErrorClass::RateLimited) {
            api_.penalize();
        }
        emit(cmd, on_failure, exec::VenueError::make(classify(err.code), err.code, err.msg));
    }

    std::expected<void, exec::VenueError> open_user_stream() {
        const auto request = api_.builder().listen_key_create();
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = api_.pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto key = parse_listen_key(*body);
        if (!key.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "listen key missing")};
        }
        state_->listen_key.assign(*key);
        state_->listen_key_due_ns = state_->clock->mono().ns + k_listen_key_interval_ns;

        if (stream_) {
            stream_->stop();
        }
        stream_ = std::make_unique<PrivateStream>(*state_, state_->listen_key);
        stream_->start();
        return {};
    }

    void send_order(const net::RestRequest& request, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = api_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            // any answer settles it; the table tracks silence
            if (!r.transport_failed()) {
                (void)unanswered_.resolve(captured.client_order_id);
            }
            if (r.transport_failed()) {
                // the request may have reached the matcher, so this is no rejection; the sweep handles it
                state_->sink->on_order_error(captured.client_order_id,
                                             exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                    0,
                                                                    r.error.empty()
                                                                        ? std::string_view{"no response from venue"}
                                                                        : std::string_view{r.error}));
                return;
            }
            const auto err = parse_rest_error(r.body);
            if (!err.has_value()) {
                emit(captured,
                     on_failure,
                     exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable error body"));
                return;
            }
            settle(captured, on_failure, *err);
        });
        if (!sent) {
            // never left this process, so the refusal is certain
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded here since the callback may never run
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    // OrderEvent has no room for text, so the sink gets the error
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
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    RestApi api_;
    std::unique_ptr<PrivateStream> stream_;
    WsApiCodec codec_{state_->signer};
    WsApiBuilder ws_builder_{state_->signer};
    std::unique_ptr<net::WsRpc> trade_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BinanceExec>(std::move(state));
}

} // namespace raikiri::venues::binance::internal
