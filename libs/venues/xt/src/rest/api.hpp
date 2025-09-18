#pragma once

// Every signed call this venue answers, over both of its deployments.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held, what the account is long. None of them belongs to the order
// path — they run on a poller's schedule.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/positions.hpp>
#include <raikiri/venues/xt/rest.hpp>

#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::xt::detail {

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Answered BEFORE the instrument table is read: the table compares
// the WHOLE id, so an id for a market that was never loaded resolves to nothing
// and would otherwise be reported as an unknown instrument — blaming a stale
// catalogue for a market this adapter simply does not serve.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("xt dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("xt options order entry");
    }
    return exec::not_implemented("xt: unknown market");
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. Missing credentials are the one exception worth
// separating — that sends an operator to the config rather than to XT's status
// page.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// The two trees report a refusal in disjoint vocabularies and disjoint
// envelopes, and spot does it at HTTP 200 — so the status is never the verdict
// there, and reading one tree's body with the other's parser loses the code
// that decides retry from halt.
exec::VenueError rest_failure(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto envelope = parse_spot_envelope(body);
        if (!envelope.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
        }
        return exec::VenueError::make(
            classify_spot(envelope->mc), numeric_code(envelope->mc), envelope->mc);
    }
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(
        classify(envelope->code), numeric_code(envelope->code), envelope->msg);
}

// True when the body says the call succeeded. Spot answers HTTP 200 to a
// refusal, so `rc` is the only thing that distinguishes them; futures answers
// a non-2xx status AND returnCode 1, and either alone is enough to fail.
bool envelope_ok(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto envelope = parse_spot_envelope(body);
        return envelope.has_value() && envelope->rc == k_spot_ok;
    }
    const auto envelope = parse_rest_envelope(body);
    return envelope.has_value() && envelope->return_code == k_ok;
}

class RestApi {
public:
    RestApi(VenueState& state, MarketType market)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state.io,
                     net::RestConfig{.host = std::string{spot_rest_host()}, .port = "443"}},
          builder_{state.signer}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {}

    // XT runs two deployments with two clocks; each offset is measured against
    // the host that will check the signature stamped from it.
    void start() {
        rest_.start();
        if (perp_) {
            sync_clock(*state_, rest_, unsigned_builder_);
        }
        if (spot_) {
            spot_rest_.start();
            sync_spot_clock(*state_, spot_rest_, unsigned_builder_);
        }
    }

    void stop() {
        rest_.stop();
        spot_rest_.stop();
    }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept {
        rest_.keepalive(now);
        spot_rest_.keepalive(now);
    }

    // The order path builds its own requests and is answered asynchronously;
    // everything else here is a synchronous read.
    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool
    send(const net::RestRequest& request, MarketType market, net::RestCallback on_done) {
        return rest_for(market).send(request, std::move(on_done));
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request, MarketType market) {
        const auto response = await_response(*state_, rest_for(market), request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, market)};
        }
        // Spot answers a refusal at HTTP 200, so the status alone is never the
        // verdict on that tree.
        if (!envelope_ok(response->body, market)) {
            return std::unexpected{rest_failure(response->body, market)};
        }
        return response->body;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<exec::OrderStatusReport> reports;
        if (market == MarketType::Spot) {
            static thread_local SpotOrderBuffer storage;
            const auto orders = parse_spot_orders(*body, storage);
            if (!orders.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
            }
            reports.reserve(orders->size());
            for (const auto& wire : *orders) {
                if (auto report = map_spot_open_order(wire, state_->instruments);
                    report.has_value()) {
                    seed(wire.order_id, *report);
                    reports.push_back(*report);
                }
            }
            return reports;
        }
        static thread_local FuturesOrderBuffer storage;
        const auto orders = parse_futures_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_futures_open_order(wire, state_->instruments);
                report.has_value()) {
                seed(wire.order_id, *report);
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market != MarketType::Spot) {
            // /future/trade/v1/order/detail is keyed by the VENUE order id and
            // XT publishes no by-client-id read, so the open-order sweep is
            // what can answer this question at all.
            return from_open_orders(id, market);
        }
        const auto request = builder_.query_order(id, market, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_spot_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_spot_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.my_trades(market, since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<exec::FillReport> reports;
        const auto push = [&reports](const OrderEvent& event) {
            reports.push_back({.exec_id = event.exec_id,
                               .exchange_order_id = event.exchange_order_id,
                               .exchange_client_order_id = event.exchange_client_order_id,
                               .instrument = event.instrument,
                               .side = event.side,
                               .price = event.price,
                               .qty = event.qty,
                               .fee = event.fee,
                               .liquidity = event.liquidity,
                               .ts_venue = event.ts_event});
        };
        if (market == MarketType::Spot) {
            static thread_local SpotTradeBuffer storage;
            const auto trades = parse_spot_trades(*body, storage);
            if (!trades.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
            }
            reports.reserve(trades->size());
            for (const auto& wire : *trades) {
                if (const auto event = map_spot_trade(
                        wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
                    event.has_value()) {
                    push(*event);
                }
            }
            return reports;
        }
        static thread_local FuturesDealBuffer storage;
        const auto deals = parse_futures_deals(*body, storage);
        if (!deals.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(deals.error()))};
        }
        reports.reserve(deals->size());
        for (const auto& wire : *deals) {
            if (const auto event = map_futures_deal(
                    wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
                event.has_value()) {
                push(*event);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // The spot wallet answers from the other host, under the other
            // envelope, with none of the field names below. Refused rather than
            // answered with the futures wallet, which would report the wrong
            // money under a spot label.
            return std::unexpected{exec::not_implemented("xt spot balance pull")};
        }
        const auto request = builder_.futures_balances(state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FuturesBalanceBuffer storage;
        const auto balances = parse_futures_balances(*body, storage);
        if (!balances.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(balances.error()))};
        }
        std::vector<BalanceReport> reports;
        reports.reserve(balances->size());
        for (const auto& wire : *balances) {
            // A row XT named no coin for is dropped rather than emitted under an
            // empty asset code.
            if (auto report = map_futures_balance(wire); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot pair pays no funding and this ledger is the FUTURES
            // wallet's. Answering with it would book a perpetual's charges
            // against a spot gateway.
            return std::unexpected{exec::not_implemented("xt spot funding charges")};
        }
        const auto request = builder_.bills(since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local BillBuffer storage;
        const auto bills = parse_bills(*body, storage);
        if (!bills.has_value()) {
            // A ledger we cannot read is NOT an empty one. Saying "no charges"
            // here books zero funding against a position that paid some.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bills.error()))};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(bills->size());
        for (const auto& wire : *bills) {
            if (const auto charge = map_bill(wire, state_->instruments, received);
                charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot balance is not a position. Answering "flat" would be a
            // claim about a concept this API does not have.
            return std::unexpected{exec::not_implemented("xt spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FuturesPositionBuffer storage;
        const auto positions = parse_futures_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_futures_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    [[nodiscard]] net::RestClient& rest_for(MarketType market) noexcept {
        return market == MarketType::Spot ? spot_rest_ : rest_;
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError>
    from_open_orders(const ExchangeClientOrderId& id, MarketType market) {
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

    void seed(std::string_view order_id, const exec::OrderStatusReport& report) {
        state_->remember(order_id,
                         {.client_order_id = report.exchange_client_order_id,
                          .leaves = report.leaves,
                          .side = report.side,
                          .side_known = true});
    }

    VenueState* state_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
    RequestBuilder unsigned_builder_;
    SignedRequestBuilder builder_;
    bool spot_{false};
    bool perp_{false};
};

} // namespace raikiri::venues::xt::detail
