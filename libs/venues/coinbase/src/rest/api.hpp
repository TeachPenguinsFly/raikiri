#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held. None of them belongs to the order path — they run on a
// poller's schedule and pump the reactor until the answer arrives, which an
// order may never do.

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

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. NotSigned is the exception, and it points at the
// config rather than at the venue.
inline exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// The body may not parse; the status classifies either way.
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

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path builds its own requests and is answered asynchronously;
    // everything else here is a synchronous read.
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
        // The id asked about is the one reported back: the UUID that travelled
        // cannot be turned into a ULID, and the caller already holds the one
        // it wanted.
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
        // GET /fills requires order_id OR product_id — "Either `order_id` or
        // `product_id` is required" — and this call names only a market.
        // Sweeping the 800-odd listed products would be a guess at which ones
        // matter, and an empty vector reads as "no fills", the one answer a
        // reconciler must not be handed wrongly.
        return std::unexpected{exec::not_implemented("coinbase fill pull is per-product")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // A coin balance is not a position: one BTC balance backs BTC-USD,
        // BTC-EUR and BTC-USDT alike, and there is no InstrumentId to report
        // it against. Answering "flat" would be a claim about a concept this
        // API does not have.
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
