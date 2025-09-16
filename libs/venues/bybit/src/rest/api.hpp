#pragma once

// Every signed call this venue answers, and the budget they are spent from.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held, what was charged. None of them belongs to the order path —
// they run on a poller's schedule and may not spend the allowance a cancel is
// about to need, which is what the Poll rate class is for.

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

#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bybit::detail {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{ep::rest_host(state.testnet)}, .port = "443"}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path builds its own requests and is answered asynchronously;
    // everything else here is a synchronous read.
    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // Local refusal, before the request exists. RateLimited rather than
    // Terminal: the caller should come back, and `retry_after_ns` says when.
    // A second's hold: long enough that a burst cannot immediately repeat,
    // short enough that one refusal does not stall reconciliation for a minute.
    void penalize(exec::RateBucket bucket) noexcept {
        if (state_->budget != nullptr) {
            state_->budget->penalize(bucket, state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    [[nodiscard]] exec::RateBudget::Costs costs() const noexcept {
        return state_->budget != nullptr ? state_->budget->costs() : exec::RateBudget::Costs{};
    }

    [[nodiscard]] std::expected<void, exec::VenueError>
    spend(exec::RateBucket bucket, std::uint32_t cost, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
        }
        const auto taken = state_->budget->take(bucket, cost, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::RateLimited,
            static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
            "local rate budget spent; retry after the stated milliseconds")};
    }

    // The envelope's fields are VIEWS INTO `body`, so the caller owns the
    // body and it outlives every use of them. Returning the two together would
    // leave the views pointing into a string that a move could relocate, and
    // returning the envelope alone — which this did — left them pointing into
    // a response destroyed on the way out. ASan caught it on the first
    // credentialed position pull; nothing without a key ever reaches here.
    [[nodiscard]] std::expected<WireRestEnvelope, exec::VenueError>
    fetch(const net::RestRequest& request,
          std::string& body,
          exec::RateBucket bucket,
          std::uint32_t cost) {
        // Poll class: these are reconciliation pulls, and they may not spend
        // the slice held back for order entry. Refused BEFORE the socket, so
        // the venue never has to say no — its refusals cost more than ours,
        // because a 429 on the order path can arrive after the matcher saw it.
        if (const auto allowed = spend(bucket, cost, exec::RateClass::Poll); !allowed.has_value()) {
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
                // THE VENUE'S ANSWER OUTRANKS OUR ACCOUNTING, which has just
                // been proven wrong. Draining the meter it refused on is what
                // stops the next pass walking into the same wall.
                penalize(bucket);
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
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local OrderUpdateBuffer storage;
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
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local ExecutionBuffer storage;
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
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local PositionBuffer storage;
        const auto positions = parse_position_list(envelope->result, storage);
        if (!positions.has_value()) {
            // NOT an empty account. A body we cannot read says nothing about
            // what is held, and answering with an empty vector here is what
            // turns "could not tell" into "is flat".
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
        const auto envelope = fetch(*request, body, exec::RateBucket::Ledger, costs().ledger);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local LedgerEntryBuffer storage;
        const auto rows = parse_funding_charges(envelope->result, storage);
        if (!rows.has_value()) {
            // A body we cannot read is NOT an empty ledger. Saying "no charges"
            // here would book zero funding against a position that paid some.
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
            // ONE ROW CARRIES BOTH COLUMNS, and which one is money depends on
            // the row's type: a TRADE states its fee and leaves funding at
            // zero, a SETTLEMENT the reverse. Reading the wrong column books a
            // zero over a real charge.
            const bool is_fee = wire.type == "TRADE";
            const auto amount = parse_decimal_e9(is_fee ? wire.fee : wire.funding);
            if (!amount.has_value() || amount->raw == 0) {
                continue; // an unparseable or absent figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            // bybit states a FEE as a cost: positive is paid away, which is the
            // opposite of how the account moved. Flipped here so both kinds
            // reach the book in one convention.
            charge.amount = is_fee ? FixedE9{-amount->raw} : *amount;
            charge.kind = is_fee ? LedgerKind::Fee : LedgerKind::Funding;
            charge.settled_at = UnixNanos{wire.transaction_ms * 1'000'000};
            // A fee names the trade it was charged on, which is what the
            // comparison keys on; the log's own row id is the fallback.
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
        // ONE WALLET behind both markets: a unified account funds a spot fill
        // and a perp's margin out of the same coins, so the answer does not
        // vary by market and narrowing it to one would be an invention.
        (void)market;
        const auto request = builder_.wallet_balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().balances);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local CoinBalanceBuffer storage;
        const auto coins = parse_wallet_balance(envelope->result, storage);
        if (!coins.has_value()) {
            // An unreadable wallet is refused rather than reported as an empty
            // vector, which reads as an account holding nothing.
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

} // namespace raikiri::venues::bybit::detail
