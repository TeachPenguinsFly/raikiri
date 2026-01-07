#pragma once

// Every signed call this venue answers, and the budget they are spent from.
//
// NOT ONE OF THEM HAS BEEN ANSWERED WITH DATA. There are no WEEX credentials
// on this machine; each path below has been exercised only as far as the
// venue's own 401, which proves the address and nothing about the body. Every
// refusal arm here is therefore written to be WRONG SAFELY: a body this
// adapter cannot read is reported as a failure to ask, never as an empty
// account.
//
// The reads are what a reconciliation asks: what is open, what filled, what is
// held, what was charged. None of them belongs to the order path — they run on
// a poller's schedule and may not spend the allowance a cancel is about to
// need, which is what the Poll rate class is for.

#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/venues/weex/map.hpp>
#include <raikiri/venues/weex/orders.hpp>
#include <raikiri/venues/weex/positions.hpp>
#include <raikiri/venues/weex/rest.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::weex::detail {

// The venue's own weight for each endpoint, from the line printed beside it.
// They differ by a factor of five, so one generic figure would either starve
// the cheap reads or overspend on the expensive one.
inline constexpr std::uint32_t k_weight_open_orders = 2;
inline constexpr std::uint32_t k_weight_fills = 5;
inline constexpr std::uint32_t k_weight_positions = 10;
inline constexpr std::uint32_t k_weight_balance = 5;
inline constexpr std::uint32_t k_weight_income = 2;

// A CANCEL IS METERED LIKE A READ, not like a place. WEEX's rule is "except for
// order PLACEMENT endpoints, all endpoints are rate limited by IP", and its
// cancel pages state a weight beside them the way the reads do — so these spend
// the same per-IP pool the market-data half spends.
inline constexpr std::uint32_t k_weight_cancel = 1;
inline constexpr std::uint32_t k_weight_cancel_batch = 5;

// PLACING AN ORDER COSTS NO IP WEIGHT. WEEX states it plainly — "0 on IP rate
// limit(X-USED-WEIGHT-1M)" — and meters placements per ACCOUNT instead, at 300
// a minute. Nothing in this process counts that budget, so a create spends the
// OrderEntry bucket, which config/venues.yaml leaves unconfigured and which an
// unconfigured bucket reads as unlimited rather than closed.
inline constexpr std::uint32_t k_cost_order = 1;

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer, state.passphrase} {}

    void start() {
        rest_.start();
        if (!signed_path_available()) {
            return; // no credentials: nothing to sign, and nothing to measure
        }
        sync_clock(*state_, rest_);
    }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool signed_path_available() const noexcept {
        return state_->signer.has_credentials() && !state_->passphrase.empty();
    }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    void penalize(exec::RateBucket bucket) noexcept {
        if (state_->budget != nullptr) {
            // A second's hold. WEEX bans for ten after a breach it notices, so
            // the cost of pausing is far below the cost of provoking one.
            state_->budget->penalize(bucket, state_->clock->mono(), 1'000'000'000ULL);
        }
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

    // A signed read, start to finish. The body is the CALLER'S, because every
    // wire type in this venue is a set of views over it: returning the views
    // alone would leave them pointing into a string destroyed on the way out.
    [[nodiscard]] std::expected<void, exec::VenueError>
    fetch(const std::expected<net::RestRequest, BuildError>& request,
          std::string& body,
          std::uint32_t weight) {
        if (!request.has_value()) {
            const auto cls = request.error() == BuildError::NotSigned
                                 ? exec::ErrorClass::AuthFailure
                                 : exec::ErrorClass::Terminal;
            return std::unexpected{exec::VenueError::make(cls, 0, to_string(request.error()))};
        }
        // Refused BEFORE the socket, so the venue never has to say no — its
        // refusals cost more than ours, because a 429 here bans this IP for ten
        // seconds and takes the market-data half down with it.
        if (const auto allowed = spend(exec::RateBucket::Shared, weight, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        auto response = await_response(*state_, rest_, *request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        body = std::move(response->body);
        // WEEX wraps nothing: a success IS the payload, and a failure is a
        // NEGATIVE `code` beside a `msg`. So the body is judged by that sign
        // and by the status line, never by an envelope field that does not
        // exist.
        const auto error = parse_rest_error(body);
        if (!error.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable body")};
        }
        if (error->code != k_ok) {
            const auto cls = classify(error->code);
            if (cls == exec::ErrorClass::RateLimited) {
                // THE VENUE'S ANSWER OUTRANKS OUR ACCOUNTING, which has just
                // been proven wrong.
                penalize(exec::RateBucket::Shared);
            }
            return std::unexpected{exec::VenueError::make(cls, error->code, error->msg)};
        }
        if (response->status < 200 || response->status >= 300) {
            // A path WEEX does not route answers a body with no `code` at all,
            // which reads to the error parser as a success payload. The status
            // line is the only thing that catches it.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, response->status, "venue refused without a code")};
        }
        return {};
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        std::string body;
        if (const auto ok = fetch(
                builder_.open_orders(market, state_->venue_now()), body, k_weight_open_orders);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_orders(body, storage);
        if (!orders.has_value()) {
            // NOT AN EMPTY BOOK. "No open orders" read off a venue that could
            // not be read would close a desk's whole book.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable order list")};
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
        // THROUGH THE LIST, because WEEX publishes no lookup by client order
        // id: GET /capi/v3/order takes `orderId` and nothing else, and this
        // process does not know the venue's id for an order it has lost.
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
        std::string body;
        if (const auto ok =
                fetch(builder_.fills(market, since, state_->venue_now()), body, k_weight_fills);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<FillBuffer>();
        auto& storage = *storage_owner;
        const auto fills = parse_fills(body, storage);
        if (!fills.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable fill list")};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            if (auto report = map_fill(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        std::string body;
        if (const auto ok =
                fetch(builder_.positions(market, state_->venue_now()), body, k_weight_positions);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();
        auto& storage = *storage_owner;
        const auto positions = parse_positions(body, storage);
        if (!positions.has_value()) {
            // NOT a flat account. A body we cannot read says nothing about what
            // is held, and answering with an empty vector here is what turns
            // "could not tell" into "is flat".
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable position list")};
        }
        // NETTED BY INSTRUMENT, not copied row for row: see map_positions.
        return map_positions(*positions, state_->instruments);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        // ONE COLLATERAL POOL behind every contract on this host, so the answer
        // does not vary by market and narrowing it would be an invention. The
        // market is still checked, because a spot gateway reaching this adapter
        // at all is a configuration nobody meant.
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("weex serves the contract tree only")};
        }
        std::string body;
        if (const auto ok = fetch(builder_.balances(state_->venue_now()), body, k_weight_balance);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BalanceBuffer>();
        auto& storage = *storage_owner;
        const auto coins = parse_balances(body, storage);
        if (!coins.has_value()) {
            // An unreadable wallet is refused rather than reported as an empty
            // vector, which reads as an account holding nothing.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable balance list")};
        }
        // The rows carry no timestamp of their own, so this is the only one
        // there is.
        const auto ts = state_->clock->wall();
        std::vector<BalanceReport> reports;
        reports.reserve(coins->size());
        for (const auto& wire : *coins) {
            if (auto report = map_balance(wire, ts); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        std::string body;
        if (const auto ok = fetch(builder_.funding_charges(market, since, state_->venue_now()),
                                  body,
                                  k_weight_income);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BillBuffer>();
        auto& storage = *storage_owner;
        const auto bills = parse_bills(body, storage);
        if (!bills.has_value()) {
            // A body we cannot read is NOT an empty ledger. Saying "no charges"
            // here would book zero funding against a position that paid some.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable bill list")};
        }
        const auto ts_init = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(bills->size());
        for (const auto& wire : *bills) {
            if (auto charge = map_funding_charge(wire, state_->instruments, ts_init);
                charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

private:
    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

} // namespace raikiri::venues::weex::detail
