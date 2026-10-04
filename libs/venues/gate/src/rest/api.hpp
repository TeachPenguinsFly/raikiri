#pragma once

// signed reconciliation reads; none is on the order path

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/gate/account.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/orders.hpp>
#include <raikiri/venues/gate/positions.hpp>
#include <raikiri/venues/gate/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::gate::internal {

// exhaustive so a new market forces a decision
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("gate delivery (dated futures) order entry");
    case MarketType::Option:
        // nothing maps an OptionInstrument onto /api/v4/options yet
        return exec::not_implemented("gate options order entry");
    }
    return exec::not_implemented("gate: unknown market");
}

exec::VenueError build_failure(BuildError err) noexcept {
    // a build refusal is terminal: rebuilding gives the same result
    return exec::VenueError::make(
        err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal, 0, to_string(err));
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // the order path builds its own requests; everything else here is a synchronous read
    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // the futures private subscribe needs this id; absent rather than refused
    [[nodiscard]] std::optional<std::uint64_t> futures_user_id() {
        const auto request = builder_.futures_account(state_->venue_now());
        if (!request.has_value()) {
            return std::nullopt;
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::nullopt;
        }
        const auto user = parse_account_user_id(*body);
        return user.has_value() ? std::optional{*user} : std::nullopt;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();

        auto& storage = *storage_owner;
        const auto orders = market == MarketType::Spot ? parse_spot_open_orders(*body, storage)
                                                       : parse_rest_orders(*body, storage, market);
        if (!orders.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
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

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.query_order(id, market, state_->venue_now());
        if (!request.has_value()) {
            // spot has no client-id lookup without the pair; the open-order sweep answers it
            return from_open_orders(id, market);
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_rest_order(*body, market);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.my_trades(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<UserTradeBuffer>();

        auto& storage = *storage_owner;
        const auto trades = parse_rest_user_trades(*body, storage, market);
        if (!trades.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(trades->size());
        for (const auto& wire : *trades) {
            const auto memo = state_->memo_for(wire.order_id);
            const auto event = map_user_trade(
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

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market != MarketType::Perp) {
            // spot balances use another endpoint and shape
            return std::unexpected{exec::not_implemented("gate spot balance pull")};
        }
        const auto request = builder_.futures_account(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto account = parse_futures_account(*body);
        if (!account.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(account.error()))};
        }
        const auto balance = map_futures_account(*account);
        if (!balance.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "gate: account names no currency")};
        }
        // one row: the futures account is per settle currency and this gateway trades one
        return std::vector<BalanceReport>{*balance};
    }

    [[nodiscard]] std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                                            UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.funding_book(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<AccountBookBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_account_book(*body, storage);
        if (!rows.has_value()) {
            // an unreadable body is an error
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& row : *rows) {
            LedgerKind kind{};
            if (row.type == "fund") {
                kind = LedgerKind::Funding;
            } else if (row.type == "fee") {
                kind = LedgerKind::Fee;
            } else {
                // pnl and transfers derive from booked fills or are not trading money
                continue;
            }
            const auto* instrument = state_->instruments.find(row.contract);
            if (instrument == nullptr) {
                continue;
            }
            const auto amount = parse_decimal_e9(row.change);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero one
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            charge.amount = *amount;
            charge.kind = kind;
            // seconds on this endpoint
            charge.settled_at = UnixNanos{row.time_s * 1'000'000'000ULL};
            // a fee keys on its trade id; the book row id is the fallback
            const auto reference = kind == LedgerKind::Fee && !row.trade_id.empty() ? row.trade_id : row.id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    [[nodiscard]] std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot has no positions
            return std::unexpected{exec::not_implemented("gate spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();

        auto& storage = *storage_owner;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
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

private:
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body)};
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

} // namespace raikiri::venues::gate::internal
