#include <raikiri/exec/order_gate.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/toobit/endpoints.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/rest.hpp>
#include <raikiri/venues/toobit/wire.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::toobit::internal {

namespace {

constexpr std::size_t k_max_unanswered = 512;
constexpr std::uint64_t k_pull_timeout_ms = 10'000;

[[nodiscard]] exec::ErrorClass classify(std::int64_t code, int status) noexcept {
    if (status == 429 || code == -1003 || code == -1015) {
        return exec::ErrorClass::RateLimited;
    }
    if (code == -1002 || code == -1107 || code == -1022 || code == -2014 || code == -2015 || status == 401) {
        return exec::ErrorClass::AuthFailure;
    }
    if (status >= 500 || code == -1001 || code == -1007) {
        return exec::ErrorClass::Retryable;
    }
    return exec::ErrorClass::Terminal;
}

[[nodiscard]] exec::VenueError build_failed(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NoCredentials ? exec::ErrorClass::AuthFailure
                                                                   : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        static thread_local ExecutionBuffer storage;
        const auto frame = parse_private(payload, storage);
        if (!frame.has_value()) {
            drop("unreadable frame");
            return;
        }
        if (frame->kind == PrivateKind::ListenKeyExpiring) {
            state_->listen_key_expiring = true;
            return;
        }
        for (const auto& row : frame->executions) {
            if (const auto event = map_execution(row, state_->instruments, received); event.has_value()) {
                state_->sink->on_order_event(*event);
            }
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

private:
    void drop(std::string_view why) {
        if (state_->private_frames_dropped == 0 && ::raikiri::log::logger() != nullptr) {
            RLOG_WARN("toobit: dropped a private frame, {}", why);
        }
        ++state_->private_frames_dropped;
    }

    VenueState* state_;
};

class PrivateStream {
public:
    PrivateStream(VenueState& state, std::string_view key)
        : handler_{state}, ws_{state.io,
                               net::WsConfig{.host = std::string{ep::k_ws_host},
                                             .port = "443",
                                             .target = std::string{ep::k_user_ws_prefix} + std::string{key}},
                               handler_} {}

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    void ping(UnixNanos now) {
        if (ws_.connected()) {
            (void)ws_.send(R"({"ping":)" + std::to_string(now.ns / 1'000'000ULL) + "}");
        }
    }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

class ToobitExec final : public exec::ExecutionClient {
public:
    explicit ToobitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_rest_host}, .port = "443"}} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    ~ToobitExec() override { state_->tick_hook = nullptr; }

    ToobitExec(const ToobitExec&) = delete;
    ToobitExec& operator=(const ToobitExec&) = delete;
    ToobitExec(ToobitExec&&) = delete;
    ToobitExec& operator=(ToobitExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        started_ = true;
        if (!state_->signer.has_credentials()) {
            return {};
        }
        return open_user_stream();
    }

    void stop() override {
        if (!started_) {
            return;
        }
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
        if (state_->signer.has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = state_->builder.listen_key_close(state_->listen_key, now()); request.has_value()) {
                (void)pull(*request, 2'000);
            }
        }
        state_->listen_key.clear();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_ != nullptr && stream_->connected(); }

    void poll(UnixNanos now_wall) noexcept override {
        rest_.keepalive(now_wall);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr || market_of(*instrument) != MarketType::Perp) {
            emit(cmd,
                 OrderEventType::Rejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "not a toobit usdt perp"));
            return;
        }
        if (const auto allowed = spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        remember(base_of(*instrument).exchange_symbol.view());
        const auto request = state_->builder.create_order(cmd, base_of(*instrument), now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        if (const auto allowed = spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request = state_->builder.cancel_order(cmd, now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::ModifyRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        if (const auto allowed = spend(ep::k_order_update, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, allowed.error());
            return;
        }
        const auto request = state_->builder.amend_order(cmd, base_of(*instrument), now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("toobit spot orders")};
        }
        const auto body = signed_pull(ep::k_open_orders, state_->builder.open_orders(now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<OrderBuffer>();
        const auto rows = parse_orders(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        std::vector<exec::OrderStatusReport> out;
        out.reserve(rows->size());
        for (const auto& row : *rows) {
            if (const auto report = map_order(row, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("toobit spot orders")};
        }
        const auto body = signed_pull(ep::k_order, state_->builder.query_order(id, now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto row = parse_order(*body);
        if (!row.has_value()) {
            return std::unexpected{unreadable(row.error())};
        }
        const auto report = map_order(*row, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unmappable order")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("toobit spot fills")};
        }
        std::vector<exec::FillReport> out;
        static thread_local const auto storage = std::make_unique<UserTradeBuffer>();
        for (const auto& symbol : state_->ordered_symbols) {
            const auto body = signed_pull(ep::k_user_trades, state_->builder.user_trades(symbol, since, now()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto rows = parse_user_trades(*body, *storage);
            if (!rows.has_value()) {
                return std::unexpected{unreadable(rows.error())};
            }
            for (const auto& row : *rows) {
                if (const auto fill = map_user_trade(row, state_->instruments); fill.has_value()) {
                    out.push_back(*fill);
                }
            }
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("toobit spot positions")};
        }
        const auto body = signed_pull(ep::k_positions, state_->builder.positions(now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        return map_positions(*rows, state_->instruments, state_->clock->wall());
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("toobit spot balances")};
        }
        const auto body = signed_pull(ep::k_balance, state_->builder.balance(now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        BalanceBuffer storage{};
        const auto rows = parse_balances(*body, storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        std::vector<BalanceReport> out;
        for (const auto& row : *rows) {
            if (const auto report = map_balance(row, state_->clock->wall()); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

private:
    [[nodiscard]] UnixNanos now() const { return state_->clock->wall(); }

    void tick(UnixNanos wall) noexcept {
        if (!started_ || !state_->signer.has_credentials()) {
            return;
        }
        if (stream_ && wall.ns >= next_ping_ns_) {
            stream_->ping(wall);
            next_ping_ns_ = wall.ns + ep::k_app_ping_interval_ns;
        }
        if (state_->listen_key_expiring || (stream_ == nullptr && wall.ns >= retry_ns_)) {
            state_->listen_key_expiring = false;
            retry_ns_ = wall.ns + ep::k_app_ping_interval_ns;
            (void)open_user_stream();
            return;
        }
        if (state_->listen_key.empty() || wall.ns < state_->listen_key_due_ns) {
            return;
        }
        state_->listen_key_due_ns = wall.ns + ep::k_listen_key_interval_ns;
        if (const auto request = state_->builder.listen_key_keepalive(state_->listen_key, wall); request.has_value()) {
            (void)rest_.send(*request, [](net::RestResponse&&) {});
        }
    }

    std::expected<void, exec::VenueError> open_user_stream() {
        const auto body = signed_pull(ep::k_listen_key, state_->builder.listen_key_create(now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto key = parse_listen_key(*body);
        if (!key.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "listen key missing")};
        }
        state_->listen_key.assign(*key);
        state_->listen_key_due_ns = now().ns + ep::k_listen_key_interval_ns;
        if (stream_) {
            stream_->stop();
        }
        stream_ = std::make_unique<PrivateStream>(*state_, state_->listen_key);
        stream_->start();
        return {};
    }

    void remember(std::string_view symbol) {
        if (std::ranges::find(state_->ordered_symbols, symbol) == state_->ordered_symbols.end()) {
            state_->ordered_symbols.emplace_back(symbol);
        }
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
        return pull(*request, k_pull_timeout_ms);
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError> pull(const net::RestRequest& request,
                                                                    std::uint64_t timeout_ms) {
        const auto response = net::await_response(state_->io, *state_->clock, rest_, request, timeout_ms);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto err = parse_rest_error(response->body);
        if (response->status != 200 || (err.has_value() && err->code != 0)) {
            const auto code = err.has_value() ? err->code : 0;
            const auto cls = classify(code, response->status);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{
                exec::VenueError::make(cls, code, err.has_value() && !err->msg.empty() ? err->msg : "http error")};
        }
        return response->body;
    }

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
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void send_order(const net::RestRequest& request, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = rest_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            if (r.transport_failed()) {
                // possibly reached the matcher; left for the sweep, not called a rejection.
                state_->sink->on_order_error(captured.client_order_id,
                                             exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                    0,
                                                                    r.error.empty() ? std::string_view{"no response"}
                                                                                    : std::string_view{r.error}));
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            const auto err = parse_rest_error(r.body);
            if (r.status == 200 && err.has_value() && err->code == 0) {
                return;
            }
            const auto code = err.has_value() ? err->code : 0;
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
                 exec::VenueError::make(cls, code, err.has_value() && !err->msg.empty() ? err->msg : "refused"));
        });
        if (!sent) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
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
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    std::unique_ptr<PrivateStream> stream_;
    bool started_{false};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t retry_ns_{0};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ToobitExec>(std::move(state));
}

} // namespace raikiri::venues::toobit::internal
