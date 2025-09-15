#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held. None of them belongs to the order path — they run on a
// poller's schedule and pump the reactor until the answer arrives, which an
// order may never do.

#include <raikiri/exec/caps.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/orders.hpp>
#include <raikiri/venues/deribit/positions.hpp>
#include <raikiri/venues/deribit/rest.hpp>

#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::deribit::detail {

// A refusal to build a request is terminal by construction: the same command
// built again produces the same refusal.
inline exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}},
          builder_{state.signer} {}

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
        std::vector<exec::OrderStatusReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.open_orders(market, currency, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto orders = parse_orders(*body, storage);
            if (!orders.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                // kind=future answers for the perpetual AND the dated series;
                // the instrument table is what separates the two markets.
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // The ULID went out as the label and comes back as one, so no local
        // table ties the OMS's id to the venue's — but the lookup is scoped by
        // currency and nothing in an id says which coin it was for.
        for (const auto currency : k_currencies) {
            const auto request = builder_.order_state_by_label(id, currency, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto orders = parse_orders(*body, storage);
            if (!orders.has_value()) {
                continue;
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    return *report;
                }
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "deribit: label not found")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        std::vector<exec::FillReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.user_trades(market, currency, since, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local UserTradeBuffer storage;
            const auto trades = parse_user_trades(*body, storage);
            if (!trades.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
            }
            for (const auto& wire : *trades) {
                auto report = map_fill_report(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        const auto now = state_->venue_now();
        std::vector<exec::PositionReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.positions(market, currency, now);
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local PositionBuffer storage;
            const auto positions = parse_positions(*body, storage);
            if (!positions.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
            }
            for (const auto& wire : *positions) {
                // Deribit stamps no time on a position row, so the read's own
                // clock is the only honest answer to "as of when".
                auto report = map_position(wire, state_->instruments, now);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
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
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "http error")};
        }
        // A JSON-RPC error rides a 200, so the envelope decides, not the
        // status line.
        const auto envelope = parse_response(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->failed()) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->error_code), envelope->error_code, envelope->error_message)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::deribit::detail
