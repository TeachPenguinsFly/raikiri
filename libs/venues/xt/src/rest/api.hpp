#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/xt/account.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/orders.hpp>
#include <raikiri/venues/xt/positions.hpp>
#include <raikiri/venues/xt/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::xt::internal {

// exhaustive so Option needs a decision; checked before the table so an unserved market is not an unknown instrument
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

// build refusals are terminal, except missing credentials
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// the trees use disjoint envelopes and codes, and spot refuses at http 200
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

// spot refuses at http 200, so rc decides; futures fails on non-2xx or returnCode 1
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

    // each tree's clock offset is measured against its own host
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

    // keeps pooled connections alive so an order does not pay a reconnect
    void keepalive(UnixNanos now) noexcept {
        rest_.keepalive(now);
        spot_rest_.keepalive(now);
    }

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
            static thread_local const auto storage_owner = std::make_unique<SpotOrderBuffer>();
            auto& storage = *storage_owner;
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
        static thread_local const auto storage_owner = std::make_unique<FuturesOrderBuffer>();
        auto& storage = *storage_owner;
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
            // order/detail is keyed by venue order id only, so a client id is answered from the open-order sweep
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
            static thread_local const auto storage_owner = std::make_unique<SpotTradeBuffer>();
            auto& storage = *storage_owner;
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
        static thread_local const auto storage_owner = std::make_unique<FuturesDealBuffer>();
        auto& storage = *storage_owner;
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
            // the spot wallet is on the other host and envelope
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
        static thread_local const auto storage_owner = std::make_unique<FuturesBalanceBuffer>();
        auto& storage = *storage_owner;
        const auto balances = parse_futures_balances(*body, storage);
        if (!balances.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(balances.error()))};
        }
        std::vector<BalanceReport> reports;
        reports.reserve(balances->size());
        for (const auto& wire : *balances) {
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
            // spot pays no funding and this ledger is the futures wallet's
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
        static thread_local const auto storage_owner = std::make_unique<BillBuffer>();
        auto& storage = *storage_owner;
        const auto bills = parse_bills(*body, storage);
        if (!bills.has_value()) {
            // an unreadable ledger is not an empty one
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
            // a spot balance is not a position
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
        static thread_local const auto storage_owner = std::make_unique<FuturesPositionBuffer>();
        auto& storage = *storage_owner;
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

} // namespace raikiri::venues::xt::internal
