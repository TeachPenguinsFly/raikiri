#pragma once

// signed reconciliation reads, spent from the poll rate class

#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/venues/bybit/rest.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bybit::internal {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{ep::rest_host(state.testnet)}, .port = "443"}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // the venue reaps idle sockets silently
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // the order path is async; everything else here is a synchronous read
    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // local refusal held for a second; RateLimited so the caller retries after retry_after_ns
    void penalize(std::string_view target) noexcept {
        if (state_->budget != nullptr) {
            state_->budget->penalize(target, state_->clock->mono(), 1'000'000'000ULL);
        }
    }


    [[nodiscard]] std::expected<void, exec::VenueError>
    spend(std::string_view target, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
        }
        const auto taken = state_->budget->take(target, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::RateLimited,
            static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
            "local rate budget spent; retry after the stated milliseconds")};
    }

    // the envelope's fields view into `body`, which the caller owns
    [[nodiscard]] std::expected<WireRestEnvelope, exec::VenueError>
    fetch(const net::RestRequest& request,
          std::string& body) {
        // poll class, refused before the socket so the venue never has to refuse
        if (const auto allowed = spend(request.target, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        body = std::move(response->body);
        const auto envelope = parse_envelope(body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope")};
        }
        if (envelope->ret_code != k_ret_ok) {
            const auto cls = classify(envelope->ret_code);
            if (cls == exec::ErrorClass::RateLimited) {
                // the venue refused: drain the meter so the next pass waits
                penalize(request.target);
            }
            return std::unexpected{
                exec::VenueError::make(cls, envelope->ret_code, envelope->ret_msg)};
        }
        return *envelope;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderUpdateBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_open_orders(envelope->result, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id.view() == id.view()) {
                return report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        const auto request = builder_.executions(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<ExecutionBuffer>();
        auto& storage = *storage_owner;
        const auto executions = parse_executions(envelope->result, storage);
        if (!executions.has_value()) {
            return std::vector<exec::FillReport>{};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(executions->size());
        for (const auto& wire : *executions) {
            const auto event = map_execution(wire, state_->instruments, state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            exec::FillReport report{};
            report.exec_id = event->exec_id;
            report.exchange_order_id = event->exchange_order_id;
            report.exchange_client_order_id = event->exchange_client_order_id;
            report.instrument = event->instrument;
            report.side = event->side;
            report.price = event->price;
            report.qty = event->qty;
            report.fee = event->fee;
            report.liquidity = event->liquidity;
            report.ts_venue = event->ts_event;
            reports.push_back(report);
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();
        auto& storage = *storage_owner;
        const auto positions = parse_position_list(envelope->result, storage);
        if (!positions.has_value()) {
            // an unreadable body is an error; an empty vector would read as flat
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable position list")};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        const auto request = builder_.funding_charges(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<LedgerEntryBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_funding_charges(envelope->result, storage);
        if (!rows.has_value()) {
            // an unreadable body is an error, not an empty ledger
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "unreadable transaction log")};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            const auto instrument = state_->instruments.find(wire.symbol);
            if (instrument == nullptr) {
                continue; // a symbol this gateway does not carry
            }
            // TRADE rows carry the fee, SETTLEMENT rows the funding
            const bool is_fee = wire.type == "TRADE";
            const auto amount = parse_decimal_e9(is_fee ? wire.fee : wire.funding);
            if (!amount.has_value() || amount->raw == 0) {
                continue; // an unparseable or absent figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            // bybit fees are positive when paid; flip to the account's direction
            charge.amount = is_fee ? FixedE9{-amount->raw} : *amount;
            charge.kind = is_fee ? LedgerKind::Fee : LedgerKind::Funding;
            charge.settled_at = UnixNanos{wire.transaction_ms * 1'000'000};
            // fees key on the trade id; the row id is the fallback
            const auto reference = is_fee && !wire.trade_id.empty() ? wire.trade_id : wire.id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        // one unified wallet backs both markets
        (void)market;
        const auto request = builder_.wallet_balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<CoinBalanceBuffer>();
        auto& storage = *storage_owner;
        const auto coins = parse_wallet_balance(envelope->result, storage);
        if (!coins.has_value()) {
            // an unreadable wallet is an error, not an empty account
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(coins.error()))};
        }
        const auto ts = ms_to_unix(envelope->time_ms, state_->clock->wall());
        std::vector<BalanceReport> reports;
        reports.reserve(coins->size());
        for (const auto& wire : *coins) {
            if (auto report = map_coin_balance(wire, ts); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

} // namespace raikiri::venues::bybit::internal
