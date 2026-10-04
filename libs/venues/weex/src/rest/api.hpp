#pragma once

// signed calls; an unreadable body is reported as a failure, never as an empty account

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

namespace raikiri::venues::weex::internal {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .pool_size = net::k_order_pool_size}},
          builder_{state.signer, state.passphrase} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // keep pooled sockets warm so an order does not pay a reconnect
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool signed_path_available() const noexcept {
        return state_->signer.has_credentials() && !state_->passphrase.empty();
    }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    void penalize(std::string_view target) noexcept {
        if (state_->budget != nullptr) {
            // weex bans for ten seconds after a breach it notices
            state_->budget->penalize(target, state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    [[nodiscard]] std::expected<void, exec::VenueError> spend(std::string_view target, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
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

    // the caller owns the body because the wire types view into it
    [[nodiscard]] std::expected<void, exec::VenueError>
    fetch(const std::expected<net::RestRequest, BuildError>& request, std::string& body) {
        if (!request.has_value()) {
            const auto cls = request.error() == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                                      : exec::ErrorClass::Terminal;
            return std::unexpected{exec::VenueError::make(cls, 0, to_string(request.error()))};
        }
        // refused before the socket; a weex 429 bans the ip for ten seconds
        if (const auto allowed = spend(request->target, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        auto response = await_response(*state_, rest_, *request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        body = std::move(response->body);
        // a negative code marks a failure; there is no envelope
        const auto error = parse_rest_error(body);
        if (!error.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable body")};
        }
        if (error->code != k_ok) {
            const auto cls = classify(error->code);
            if (cls == exec::ErrorClass::RateLimited) {
                // the venue's refusal overrides local accounting
                penalize(request->target);
            }
            return std::unexpected{exec::VenueError::make(cls, error->code, error->msg)};
        }
        if (response->status < 200 || response->status >= 300) {
            // an unrouted path has no code; only the status line catches it
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "venue refused without a code")};
        }
        return {};
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        std::string body;
        if (const auto ok = fetch(builder_.open_orders(market, state_->venue_now()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_orders(body, storage);
        if (!orders.has_value()) {
            // an unreadable body must not read as no open orders
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable order list")};
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
        // weex has no lookup by client id, so go through the open list
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

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos since) {
        std::string body;
        if (const auto ok = fetch(builder_.fills(market, since, state_->venue_now()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<FillBuffer>();
        auto& storage = *storage_owner;
        const auto fills = parse_fills(body, storage);
        if (!fills.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable fill list")};
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

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
        std::string body;
        if (const auto ok = fetch(builder_.positions(market, state_->venue_now()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();
        auto& storage = *storage_owner;
        const auto positions = parse_positions(body, storage);
        if (!positions.has_value()) {
            // an unreadable body must not read as flat
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable position list")};
        }
        // netted by instrument; see map_positions
        return map_positions(*positions, state_->instruments);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        // one collateral pool backs every contract; other markets are a misconfiguration
        if (market != MarketType::Perp) {
            return std::unexpected{exec::not_implemented("weex serves the contract tree only")};
        }
        std::string body;
        if (const auto ok = fetch(builder_.balances(state_->venue_now()), body); !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BalanceBuffer>();
        auto& storage = *storage_owner;
        const auto coins = parse_balances(body, storage);
        if (!coins.has_value()) {
            // an unreadable wallet must not read as empty
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable balance list")};
        }
        // rows carry no timestamp
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

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        std::string body;
        if (const auto ok = fetch(builder_.funding_charges(market, since, state_->venue_now()), body);
            !ok.has_value()) {
            return std::unexpected{ok.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BillBuffer>();
        auto& storage = *storage_owner;
        const auto bills = parse_bills(body, storage);
        if (!bills.has_value()) {
            // an unreadable body must not read as no charges
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable bill list")};
        }
        const auto ts_init = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(bills->size());
        for (const auto& wire : *bills) {
            if (auto charge = map_funding_charge(wire, state_->instruments, ts_init); charge.has_value()) {
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

} // namespace raikiri::venues::weex::internal
