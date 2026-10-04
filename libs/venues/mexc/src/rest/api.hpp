#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/mexc/account.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/orders.hpp>
#include <raikiri/venues/mexc/positions.hpp>
#include <raikiri/venues/mexc/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::mexc::internal {

exec::VenueError build_failure(BuildError err) noexcept {
    // a build refusal repeats on retry, so it is terminal
    return exec::VenueError::make(
        err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal, 0, to_string(err));
}

// an empty answer would read as the venue holding nothing
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("mexc dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("mexc options order entry");
    }
    return exec::not_implemented("mexc: unknown market");
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{private_rest_host()}, .pool_size = net::k_order_pool_size}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // the venue reaps idle sockets silently
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /api/v3/openOrders requires a symbol and has no account-wide form
            return std::unexpected{exec::not_implemented("mexc spot open-order sweep (openOrders is per-symbol)")};
        }
        const auto request = builder_.open_orders(market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FuturesOrderBuffer>();

        auto& storage = *storage_owner;
        const auto orders = parse_futures_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_futures_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Perp) {
            // the futures single-order read needs a symbol, so the open-order sweep answers instead
            return from_open_orders(id, market);
        }
        const auto request = builder_.query_order(id, market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_spot_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_spot_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /api/v3/myTrades is per-symbol; an empty vector would read as nothing traded
            return std::unexpected{exec::not_implemented("mexc spot fill pull (myTrades is per-symbol)")};
        }
        const auto request = builder_.my_trades(market, {}, since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FuturesDealBuffer>();

        auto& storage = *storage_owner;
        const auto deals = parse_futures_deals(*body, storage);
        if (!deals.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(deals.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(deals->size());
        for (const auto& wire : *deals) {
            const auto memo = state_->memo_for(wire.order_id);
            const auto event = map_futures_deal(
                wire, state_->instruments, memo.leaves, memo.client_order_id, state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            reports.push_back({.exec_id = event->exec_id,
                               .exchange_order_id = event->exchange_order_id,
                               .exchange_client_order_id = event->exchange_client_order_id,
                               .instrument = event->instrument,
                               .side = event->side,
                               .price = event->price,
                               .qty = event->qty,
                               .fee = event->fee,
                               .liquidity = event->liquidity,
                               .ts_venue = event->ts_event});
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot pays no funding; this ledger is futures-only
            return std::unexpected{exec::not_implemented("mexc spot funding charges")};
        }
        const auto request = builder_.funding_records(market, since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FundingRecordBuffer>();

        auto& storage = *storage_owner;
        const auto rows = parse_funding_records(*body, storage);
        if (!rows.has_value()) {
            // an unreadable ledger must not read as no funding
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto charge = map_funding_record(wire, state_->instruments, received); charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot balances are not positions
            return std::unexpected{exec::not_implemented("mexc spot position pull")};
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
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
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& request,
                                                                     MarketType market) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, market)};
        }
        // futures answers 200 with success:false
        if (market != MarketType::Spot) {
            const auto envelope = parse_rest_envelope(response->body);
            if (!envelope.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable envelope")};
            }
            if (!envelope->success || envelope->code != k_ok) {
                return std::unexpected{exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
            }
        }
        return response->body;
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
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::mexc::internal
