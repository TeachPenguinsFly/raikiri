#pragma once

// signed reconciliation reads; they pump the reactor until answered

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/coinbase/envelope.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/orders.hpp>
#include <raikiri/venues/coinbase/rest.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::coinbase::internal {

// build refusals are terminal; NotSigned points at the config
inline exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// the status classifies even when the body does not parse
inline exec::VenueError http_failure(const net::RestResponse& response) noexcept {
    const auto message = parse_rest_error(response.body);
    return exec::VenueError::make(classify_http(response.status),
                                  response.status,
                                  message.has_value() ? *message : std::string_view{"http error"});
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer, state.passphrase} {}

    void start() {
        rest_.start();
        sync_clock(*state_, rest_);
    }

    void stop() { rest_.stop(); }

    // sweeps connections the venue closed so an idle gateway does not write into a dead socket
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // the order path is async; everything else here is a synchronous read
    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
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
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report =
                    map_open_order(wire, state_->instruments, state_->recall(wire.client_oid));
                report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        const auto request = builder_.query_order(id, market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto order = parse_order(*body);
        if (!order.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(order.error()))};
        }
        // the uuid cannot be turned back into a ulid, so the caller's id is reported
        auto report = map_open_order(*order, state_->instruments, id);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        (void)since;
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // GET /fills needs order_id or product_id, and this call names only a market
        return std::unexpected{exec::not_implemented("coinbase fill pull is per-product")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // a coin balance backs many products and maps to no position
        return std::unexpected{exec::not_implemented("coinbase spot position pull")};
    }

private:
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& req) {
        const auto response = await_response(*state_, rest_, req);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{http_failure(*response)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::coinbase::internal
