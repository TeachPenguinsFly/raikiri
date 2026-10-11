// usdt perpetual orders over signed rest, the only order transport blofin offers; the private
// orders channel carries the lifecycle and, by its cumulative fill size, the fills

#include <raikiri/exec/order_gate.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/blofin/endpoints.hpp>
#include <raikiri/venues/blofin/trade.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::blofin::internal {

namespace {

constexpr std::size_t k_max_unanswered = 512;
constexpr std::uint64_t k_pull_timeout_ms = 10'000;
// the server drops a socket silent for 30 s
constexpr std::uint64_t k_private_ping_ns = 20'000'000'000ULL;

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
        const auto login = login_frame(state_->signer, state_->passphrase, state_->clock->wall());
        if (!login.has_value() || !client_->send(*login)) {
            connectivity(exec::ConnState::AuthFailed);
        }
        // subscribing before the login answer is refused with 60011
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        static thread_local const auto storage = std::make_unique<OrderBuffer>();
        const auto frame = parse_private(payload, *storage);
        if (!frame.has_value()) {
            if (state_->private_frames_dropped++ == 0 && ::raikiri::log::logger() != nullptr) {
                RLOG_WARN("blofin: dropped an unreadable private frame");
            }
            return;
        }
        switch (frame->kind) {
        case PrivateKind::Login:
            (void)client_->send(subscribe_orders_frame());
            connectivity(exec::ConnState::Connected);
            break;
        case PrivateKind::LoginFailed:
            state_->last_error_code = frame->code;
            connectivity(exec::ConnState::AuthFailed);
            break;
        case PrivateKind::Snapshot:
        case PrivateKind::Orders: {
            std::array<OrderEvent, 2> events{};
            for (const auto& row : frame->orders) {
                std::size_t count = 0;
                map_order_push(row,
                               state_->instruments,
                               state_->progress,
                               received,
                               events,
                               count,
                               frame->kind == PrivateKind::Snapshot);
                for (std::size_t i = 0; i < count; ++i) {
                    state_->sink->on_order_event(events[i]);
                }
            }
            break;
        }
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
        : handler_{state},
          ws_{state.io,
              net::WsConfig{
                  .host = std::string{ep::host(state.testnet)}, .port = "443", .target = std::string{k_ws_private}},
              handler_} {
        handler_.attach(ws_);
    }

    void start() { ws_.start(); }

    void stop() { ws_.stop(); }

    void ping() {
        if (ws_.connected()) {
            (void)ws_.send("ping");
        }
    }

    [[nodiscard]] bool connected() const noexcept { return ws_.connected(); }

private:
    PrivateHandler handler_;
    net::WsClient ws_;
};

class BlofinExec final : public exec::ExecutionClient {
public:
    explicit BlofinExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::host(state_->testnet)}, .port = "443"}} {}

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        if (state_->signer.has_credentials() && !state_->passphrase.empty()) {
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
            stream_->ping();
            next_ping_ns_ = now.ns + k_private_ping_ns;
        }
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* base = known(cmd, OrderEventType::Rejected);
        if (base == nullptr || !admitted(cmd, OrderEventType::Rejected)) {
            return;
        }
        remember(base->exchange_symbol.view());
        send_built(builder_.create_order(cmd, *base, state_->clock->wall()), cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto* base = known(cmd, OrderEventType::CancelRejected);
        if (base == nullptr || !admitted(cmd, OrderEventType::CancelRejected)) {
            return;
        }
        send_built(builder_.cancel_order(cmd, base->exchange_symbol.view(), state_->clock->wall()),
                   cmd,
                   OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* base = known(cmd, OrderEventType::ModifyRejected);
        if (base == nullptr || !admitted(cmd, OrderEventType::ModifyRejected)) {
            return;
        }
        send_built(builder_.amend_order(cmd, *base, state_->clock->wall()), cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) override {
        const auto body = signed_pull(k_orders_pending, builder_.orders_pending(state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<OrderBuffer>();
        const auto rows = parse_orders(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        std::vector<exec::OrderStatusReport> out;
        for (const auto& row : *rows) {
            if (const auto report = map_order_report(row, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType) override {
        static thread_local const auto storage = std::make_unique<OrderBuffer>();
        for (const auto& symbol : symbols_) {
            const auto body = signed_pull(k_order_detail, builder_.order_detail(symbol, id, state_->clock->wall()));
            if (!body.has_value()) {
                continue; // asked on the wrong contract
            }
            const auto rows = parse_orders(*body, *storage);
            if (!rows.has_value()) {
                continue;
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

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType, UnixNanos since) override {
        const auto body = signed_pull(k_fills_history, builder_.fills(since, state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<FillBuffer>();
        const auto rows = parse_fills(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        std::vector<exec::FillReport> out;
        for (const auto& row : *rows) {
            if (const auto fill = map_fill(row, state_->instruments); fill.has_value()) {
                out.push_back(*fill);
            }
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType) override {
        const auto body = signed_pull(k_positions, builder_.positions(state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable(rows.error())};
        }
        std::vector<exec::PositionReport> out;
        for (const auto& row : *rows) {
            if (const auto report = map_position(row, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType) override {
        const auto body = signed_pull(k_balance, builder_.balance(state_->clock->wall()));
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
    [[nodiscard]] const InstrumentBase* known(const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return nullptr;
        }
        return &base_of(*instrument);
    }

    // one trading budget: 30 requests per 10 s per user across place, cancel and amend
    [[nodiscard]] bool admitted(const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        if (state_->budget == nullptr) {
            return true;
        }
        const auto taken = state_->budget->take(k_place_order, state_->clock->mono(), exec::RateClass::Order);
        if (taken.has_value()) {
            return true;
        }
        emit(cmd,
             on_failure,
             exec::VenueError::make(exec::ErrorClass::RateLimited,
                                    static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
                                    "local rate budget spent; retry after the stated milliseconds"));
        return false;
    }

    void remember(std::string_view symbol) {
        if (std::ranges::find(symbols_, symbol) == symbols_.end()) {
            symbols_.emplace_back(symbol);
        }
    }

    void penalize() const noexcept {
        if (state_->budget != nullptr) {
            state_->budget->penalize_all(state_->clock->mono(), ep::k_rest_suspension_ns);
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
        if (state_->budget != nullptr) {
            if (const auto taken = state_->budget->take(target, state_->clock->mono(), exec::RateClass::Poll);
                !taken.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::RateLimited,
                                           static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
                                           "local rate budget spent")};
            }
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

    void send_built(const std::expected<net::RestRequest, BuildError>& request,
                    const OrderCmd& cmd,
                    OrderEventType on_failure) noexcept {
        if (!request.has_value()) {
            emit(cmd, on_failure, build_failed(request.error()));
            return;
        }
        const auto captured = cmd;
        const auto sent = rest_.send(*request, [this, captured, on_failure](net::RestResponse&& r) {
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
                return; // the orders channel reports what happens next
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
    RequestBuilder builder_{state_->signer, state_->passphrase};
    std::unique_ptr<PrivateStream> stream_;
    std::vector<std::string> symbols_;
    std::uint64_t next_ping_ns_{0};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BlofinExec>(std::move(state));
}

} // namespace raikiri::venues::blofin::internal
