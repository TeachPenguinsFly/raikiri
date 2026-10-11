// usdt perpetual orders over signed rest, the only order transport phemex offers; the aop_p socket
// carries the lifecycle and the fills, rest rejections excepted

#include <raikiri/exec/order_gate.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/phemex/endpoints.hpp>
#include <raikiri/venues/phemex/trade.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::phemex::internal {

namespace {

constexpr std::size_t k_max_unanswered = 512;
constexpr std::uint64_t k_pull_timeout_ms = 10'000;
// the socket closes after 30 s without a ping
constexpr std::uint64_t k_private_ping_ns = 10'000'000'000ULL;
constexpr std::uint64_t k_auth_id = 1;
constexpr std::uint64_t k_subscribe_id = 2;

[[nodiscard]] exec::VenueError build_failed(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NoCredentials ? exec::ErrorClass::AuthFailure
                                                                   : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

[[nodiscard]] exec::VenueError linear_only() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "phemex orders reach usdt perpetuals only");
}

// authenticates, subscribes once the venue says yes, and maps orders_p rows to sink events
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto auth = auth_frame(state_->signer, state_->clock->wall(), k_auth_id);
        if (!auth.has_value() || !client_->send(*auth)) {
            connectivity(exec::ConnState::AuthFailed);
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        static thread_local const auto storage = std::make_unique<OrderRowBuffer>();
        const auto frame = parse_private(payload, *storage);
        if (!frame.has_value()) {
            if (state_->private_frames_dropped++ == 0 && ::raikiri::log::logger() != nullptr) {
                RLOG_WARN("phemex: dropped an unreadable private frame");
            }
            return;
        }
        switch (frame->kind) {
        case PrivateKind::Reply:
            if (frame->id == k_auth_id) {
                if (frame->error_code != 0) {
                    state_->last_error_code = frame->error_code;
                    connectivity(exec::ConnState::AuthFailed);
                    return;
                }
                (void)client_->send(subscribe_frame(k_subscribe_id));
            } else if (frame->id == k_subscribe_id) {
                connectivity(frame->error_code == 0 ? exec::ConnState::Connected : exec::ConnState::AuthFailed);
            }
            break;
        case PrivateKind::Orders:
            for (const auto& row : frame->orders) {
                if (const auto event = map_order_row(row, state_->instruments, received); event.has_value()) {
                    state_->sink->on_order_event(*event);
                }
            }
            break;
        case PrivateKind::Snapshot: // the open orders as they stand; reconciliation reads them over rest
        case PrivateKind::Other:
            break;
        }
    }

    void on_close(net::WsCloseReason) override { connectivity(exec::ConnState::Disconnected); }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void connectivity(exec::ConnState conn) {
        state_->sink->on_connectivity({.state = conn, .stream_id = 1, .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class PrivateStream {
public:
    explicit PrivateStream(VenueState& state)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{ep::stream_host(state.testnet)},
                                             .port = "443",
                                             .target = std::string{ep::stream_path(state.testnet)}},
                               handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    void ping(std::uint64_t id) {
        if (ws_.connected()) {
            (void)ws_.send(R"({"id":)" + std::to_string(id) + R"(,"method":"server.ping","params":[]})");
        }
    }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

class PhemexExec final : public exec::ExecutionClient {
public:
    explicit PhemexExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        if (state_->signer.has_credentials() && state_->market == MarketType::Perp) {
            stream_ = std::make_unique<PrivateStream>(*state_);
            stream_->start();
        }
        return {};
    }

    void stop() override {
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
        rest_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_ != nullptr && stream_->connected(); }

    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        if (stream_ && now.ns >= next_ping_ns_) {
            stream_->ping(++ping_id_ + k_subscribe_id);
            next_ping_ns_ = now.ns + k_private_ping_ns;
        }
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* base = tradable(cmd, OrderEventType::Rejected);
        if (base == nullptr) {
            return;
        }
        if (const auto allowed = spend(k_create_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        remember(base->exchange_symbol.view());
        const auto request = builder_.create_order(cmd, *base, state_->clock->wall());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto* base = tradable(cmd, OrderEventType::CancelRejected);
        if (base == nullptr) {
            return;
        }
        if (const auto allowed = spend(k_cancel_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request = builder_.cancel_order(cmd, base->exchange_symbol.view(), state_->clock->wall());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* base = tradable(cmd, OrderEventType::ModifyRejected);
        if (base == nullptr) {
            return;
        }
        if (const auto allowed = spend(k_replace_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, allowed.error());
            return;
        }
        const auto request = builder_.amend_order(cmd, *base, state_->clock->wall());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

    // per symbol: the ones this process traded and the ones holding a position
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{linear_only()};
        }
        if (const auto held = position_reports(market); !held.has_value()) {
            return std::unexpected{held.error()};
        }
        std::vector<exec::OrderStatusReport> out;
        static thread_local const auto storage = std::make_unique<OrderRowBuffer>();
        for (const auto& symbol : symbols_) {
            const auto body =
                signed_pull(k_active_orders, builder_.open_orders(symbol, state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto rows = parse_order_rows(*body, *storage);
            if (!rows.has_value()) {
                return std::unexpected{unreadable(rows.error())};
            }
            for (const auto& row : *rows) {
                if (const auto report = map_order_report(row, state_->instruments); report.has_value()) {
                    out.push_back(*report);
                }
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{linear_only()};
        }
        static thread_local const auto storage = std::make_unique<OrderRowBuffer>();
        for (const auto& symbol : symbols_) {
            const auto body =
                signed_pull(k_order_by_id, builder_.query_order(symbol, id, state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto rows = parse_order_rows(*body, *storage);
            if (!rows.has_value()) {
                return std::unexpected{unreadable(rows.error())};
            }
            for (const auto& row : *rows) {
                if (const auto report = map_order_report(row, state_->instruments);
                    report.has_value() && report->exchange_client_order_id.view() == id.view()) {
                    return *report;
                }
            }
        }
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not found")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        if (market != MarketType::Perp) {
            return std::unexpected{linear_only()};
        }
        std::vector<exec::FillReport> out;
        static thread_local const auto storage = std::make_unique<OrderRowBuffer>();
        for (const auto& symbol : symbols_) {
            const auto body =
                signed_pull(k_user_trades, builder_.user_trades(symbol, since, state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto rows = parse_order_rows(*body, *storage);
            if (!rows.has_value()) {
                return std::unexpected{unreadable(rows.error())};
            }
            for (const auto& row : *rows) {
                if (const auto fill = map_fill_report(row, state_->instruments); fill.has_value()) {
                    out.push_back(*fill);
                }
            }
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{linear_only()};
        }
        const auto body = account_positions();
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PositionBuffer>();
        const auto parsed = parse_account_positions(*body, *storage);
        if (!parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        std::vector<exec::PositionReport> out;
        for (const auto& row : parsed->positions) {
            if (const auto report = map_position(row, state_->instruments); report.has_value()) {
                if (report->net_qty.raw != 0) {
                    remember(row.symbol);
                }
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{linear_only()};
        }
        const auto body = account_positions();
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PositionBuffer>();
        const auto parsed = parse_account_positions(*body, *storage);
        if (!parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        std::vector<BalanceReport> out;
        if (const auto report = map_balance(parsed->account, state_->clock->wall()); report.has_value()) {
            out.push_back(*report);
        }
        return out;
    }

private:
    [[nodiscard]] const InstrumentBase* tradable(const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr || base_of(*instrument).kind != ContractKind::Linear ||
            cmd.instrument.market() != MarketType::Perp) {
            emit(cmd, on_failure, linear_only());
            return nullptr;
        }
        return &base_of(*instrument);
    }

    void remember(std::string_view symbol) {
        if (std::ranges::find(symbols_, symbol) == symbols_.end()) {
            symbols_.emplace_back(symbol);
        }
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError> account_positions() {
        return signed_pull(k_account_positions, builder_.account_positions(state_->clock->wall()));
    }

    [[nodiscard]] std::expected<void, exec::VenueError> spend(std::string_view target, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {};
        }
        const auto taken = state_->budget->take(target, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::RateLimited,
                                   static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
                                   "local rate budget spent; retry after the stated milliseconds")};
    }

    void penalize() const noexcept {
        if (state_->budget != nullptr) {
            state_->budget->penalize_all(state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    [[nodiscard]] static exec::VenueError unreadable(WireError err) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, wire::to_string(err));
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    signed_pull(std::string_view target, const std::expected<net::RestRequest, BuildError>& request) {
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        if (const auto allowed = spend(target, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto response = net::await_response(state_->io, *state_->clock, rest_, *request, k_pull_timeout_ms);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto reply = parse_reply(response->body);
        if (response->status != 200 || !reply.has_value() || reply->code != k_ok) {
            const auto code = reply.has_value() ? reply->code : 0;
            const auto cls = classify(code, response->status);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{exec::VenueError::make(
                cls, code, reply.has_value() && !reply->msg.empty() ? reply->msg : std::string_view{"http error"})};
        }
        return response->body;
    }

    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
        }
    }

    void send_order(const net::RestRequest& request, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = rest_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            if (r.transport_failed()) {
                // possibly reached the matcher; left for the sweep, not called a rejection
                state_->sink->on_order_error(captured.client_order_id,
                                             exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                    0,
                                                                    r.error.empty() ? std::string_view{"no response"}
                                                                                    : std::string_view{r.error}));
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            const auto reply = parse_reply(r.body);
            if (r.status == 200 && reply.has_value() && reply->code == k_ok) {
                return; // the socket reports what happens next
            }
            const auto code = reply.has_value() ? reply->code : 0;
            if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(code)) {
                return;
            }
            const auto cls = classify(code, r.status);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            if (r.status >= 500) {
                state_->sink->on_order_error(captured.client_order_id,
                                             exec::VenueError::make(cls, code, "venue error; outcome unknown"));
                return;
            }
            emit(captured,
                 on_failure,
                 exec::VenueError::make(
                     cls, code, reply.has_value() && !reply->msg.empty() ? reply->msg : std::string_view{"refused"}));
        });
        if (!sent) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
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
    net::RestClient rest_;
    RequestBuilder builder_{state_->signer};
    std::unique_ptr<PrivateStream> stream_;
    std::vector<std::string> symbols_;
    std::uint64_t next_ping_ns_{0};
    std::uint64_t ping_id_{0};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<PhemexExec>(std::move(state));
}

} // namespace raikiri::venues::phemex::internal
