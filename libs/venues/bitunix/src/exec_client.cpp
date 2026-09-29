#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/orders.hpp>
#include <raikiri/venues/bitunix/trade.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitunix::internal {

namespace {

constexpr std::size_t k_max_unanswered = 512;

enum class Kind : std::uint8_t { Create, Cancel, Amend };

[[nodiscard]] OrderEventType reject_type(Kind kind) noexcept {
    return kind == Kind::Create   ? OrderEventType::Rejected
           : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                  : OrderEventType::ModifyRejected;
}

[[nodiscard]] exec::VenueError build_failure(BuildError why) noexcept {
    return exec::VenueError::make(
        why == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal, 0, to_string(why));
}

// the venue answers an overload and most refusals as http 200 with a nonzero code
[[nodiscard]] std::optional<exec::VenueError> failure_of(const net::RestResponse& response) {
    const auto status = parse_status(response.body);
    if (!status.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, response.status, "unreadable answer");
    }
    if (status->code != k_ok) {
        return exec::VenueError::make(classify(status->code), status->code, status->msg);
    }
    if (response.status != 200) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, response.status, "venue refused without a code");
    }
    return std::nullopt;
}

class BitunixExecution final : public exec::ExecutionClient {
public:
    explicit BitunixExecution(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_perp_host}, .pool_size = 4}},
          orders_{state_->signer}, reads_{state_->signer} {}

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return started_ && state_->signer.has_credentials(); }

    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        fire(cmd, Kind::Create, orders_.place(cmd, state_->instruments, state_->clock->wall()));
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        fire(cmd, Kind::Cancel, orders_.cancel(cmd, state_->instruments, state_->clock->wall()));
    }

    void amend(const OrderCmd& cmd) noexcept override {
        fire(cmd, Kind::Amend, orders_.modify(cmd, state_->instruments, state_->clock->wall()));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        std::string body;
        if (const auto ok = fetch(market, reads_.pending_orders(state_->clock->wall()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage = std::make_unique<OrderBuffer>();
        const auto rows = parse_orders(body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable("order list")};
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
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id.view() == id.view()) {
                return report;
            }
        }
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        std::string body;
        const auto request = reads_.history_trades(since, state_->clock->wall());
        if (const auto ok = fetch(market, request, body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage = std::make_unique<FillBuffer>();
        const auto rows = parse_fills(body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable("fill list")};
        }
        std::vector<exec::FillReport> out;
        out.reserve(rows->size());
        for (const auto& row : *rows) {
            if (const auto fill = map_fill(row, state_->instruments); fill.has_value()) {
                out.push_back(*fill);
            }
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        std::string body;
        if (const auto ok = fetch(market, reads_.positions(state_->clock->wall()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable("position list")};
        }
        std::vector<exec::PositionReport> out;
        out.reserve(rows->size());
        for (const auto& row : *rows) {
            if (const auto position = map_position(row, state_->instruments); position.has_value()) {
                out.push_back(*position);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        std::string body;
        if (const auto ok = fetch(market, reads_.account(state_->clock->wall()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        BalanceBuffer storage;
        const auto rows = parse_balances(body, storage);
        if (!rows.has_value()) {
            return std::unexpected{unreadable("account")};
        }
        std::vector<BalanceReport> out;
        const auto ts = state_->clock->wall();
        for (const auto& row : *rows) {
            if (const auto balance = map_balance(row, ts); balance.has_value()) {
                out.push_back(*balance);
            }
        }
        return out;
    }

private:
    [[nodiscard]] static exec::VenueError unreadable(std::string_view what) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, what);
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

    [[nodiscard]] std::expected<void, exec::VenueError>
    fetch(MarketType market, const std::expected<net::RestRequest, BuildError>& request, std::string& body) {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("bitunix order entry serves perpetuals only")};
        }
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        if (const auto allowed = spend(request->target, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        auto response = net::await_response(state_->io, *state_->clock, rest_, *request, rest_.max_wait_ms() + 1'000);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (const auto failure = failure_of(*response); failure.has_value()) {
            return std::unexpected{*failure};
        }
        body = std::move(response->body);
        return {};
    }

    void fire(const OrderCmd& cmd, Kind kind, const std::expected<net::RestRequest, BuildError>& request) noexcept {
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (const auto allowed = spend(request->target, exec::RateClass::Order); !allowed.has_value()) {
            emit_reject(cmd, kind, allowed.error());
            return;
        }
        const auto copy = cmd;
        if (!rest_.send(*request,
                        [this, copy, kind](net::RestResponse&& response) { on_response(response, copy, kind); })) {
            emit_reject(cmd, kind, exec::VenueError::make(exec::ErrorClass::Terminal, 0, "rest queue full"));
            return;
        }
        (void)in_flight_.add({.client_order_id = cmd.client_order_id,
                              .exchange_client_order_id = cmd.exchange_client_order_id,
                              .instrument = cmd.instrument,
                              .side = cmd.side,
                              .qty = cmd.qty,
                              .kind = cmd.kind,
                              .sent = state_->clock->mono()});
    }

    void on_response(const net::RestResponse& response, const OrderCmd& cmd, Kind kind) noexcept {
        if (response.transport_failed()) {
            state_->sink->on_order_error(cmd.client_order_id,
                                         exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                0,
                                                                response.error.empty()
                                                                    ? std::string_view{"no response from venue"}
                                                                    : std::string_view{response.error}));
            return;
        }
        (void)in_flight_.resolve(cmd.client_order_id);
        if (const auto failure = failure_of(response); failure.has_value()) {
            emit_reject(cmd, kind, *failure);
            return;
        }
        if (kind == Kind::Cancel) {
            on_cancel(response, cmd);
            return;
        }
        const auto ack = parse_ack(response.body);
        if (!ack.has_value()) {
            emit_reject(cmd, kind, unreadable("unreadable order acknowledgement"));
            return;
        }
        emit(cmd, kind == Kind::Create ? OrderEventType::Accepted : OrderEventType::Updated, ack->order_id, cmd.qty);
    }

    void on_cancel(const net::RestResponse& response, const OrderCmd& cmd) noexcept {
        CancelBuffer storage;
        const auto rows = parse_cancels(response.body, storage);
        if (!rows.has_value() || rows->empty()) {
            emit_reject(cmd, Kind::Cancel, unreadable("unreadable cancel answer"));
            return;
        }
        const auto& row = rows->front();
        if (!row.ok) {
            emit_reject(cmd, Kind::Cancel, exec::VenueError::make(classify(row.error_code), row.error_code, row.error));
            return;
        }
        emit(cmd, OrderEventType::Canceled, row.ack.order_id, QtyLots::from_raw(0));
    }

    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : in_flight_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
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

    void emit(const OrderCmd& cmd, OrderEventType type, std::string_view order_id, QtyLots leaves) noexcept {
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
        if (const auto id = ExchangeOrderId::from(order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.leaves_qty = leaves;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = reject_type(kind);
        event.instrument = cmd.instrument;
        event.side = cmd.side;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    RequestBuilder orders_;
    RequestBuilder reads_;
    exec::InFlightTable in_flight_{k_max_unanswered};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitunixExecution>(std::move(state));
}

} // namespace raikiri::venues::bitunix::internal
