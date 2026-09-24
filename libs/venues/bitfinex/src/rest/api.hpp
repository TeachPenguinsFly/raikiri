#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitfinex/envelope.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/orders.hpp>
#include <raikiri/venues/bitfinex/positions.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bitfinex::internal {

// exhaustive over markets; checked before the instrument table
inline std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("bitfinex dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("bitfinex options order entry");
    }
    return exec::not_implemented("bitfinex: unknown market");
}

// build refusals are terminal except missing credentials
inline exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// ["error",code,text]; told apart from data by its first element
inline std::optional<exec::VenueError> rest_error(std::string_view body) {
    if (!is_rest_error(body)) {
        return std::nullopt;
    }
    const auto err = parse_rest_error(body);
    if (!err.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err.error()));
    }
    return exec::VenueError::make(classify(err->code), err->code, err->msg);
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{private_rest_host()}, .port = "443"}},
          builder_{state.api_key, state.api_secret} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // sweeps dead rest connections so an idle gateway does not send into a closed socket
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, state_->clock->wall());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            auto report = map_open_order(wire, state_->instruments);
            if (!report.has_value()) {
                continue;
            }
            // /auth/r/orders is account-wide; drop the other market's rows
            if (report->instrument.market() != market) {
                continue;
            }
            report->exchange_client_order_id = state_->client_id_for(wire);
            state_->remember(
                wire.id,
                {.client_order_id = report->exchange_client_order_id, .leaves = report->leaves});
            reports.push_back(*report);
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // no read by client id and cid is not invertible, so the open-order sweep answers
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
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.my_trades(market, since, state_->clock->wall());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<ExecutionBuffer>();
        auto& storage = *storage_owner;
        const auto fills = parse_executions(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            const auto event = map_execution(
                wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
            if (!event.has_value() || event->instrument.market() != market) {
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

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            return std::unexpected{exec::not_implemented("bitfinex spot position pull")};
        }
        const auto request = builder_.positions(market, state_->clock->wall());
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
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
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
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& req) {
        const auto response = await_response(*state_, rest_, req);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (const auto failed = rest_error(response->body); failed.has_value()) {
            return std::unexpected{*failed};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, response->status, "http error")};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::bitfinex::internal
