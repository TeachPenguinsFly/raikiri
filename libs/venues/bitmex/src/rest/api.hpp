#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what the account is long. None of them belongs to the order path — they run
// on a poller's schedule.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/orders.hpp>
#include <raikiri/venues/bitmex/positions.hpp>
#include <raikiri/venues/bitmex/rest.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bitmex::detail {

// The body may not parse; the status classifies either way.
exec::VenueError rest_failure(const net::RestResponse& response) {
    const auto detail = parse_rest_error(response.body);
    const auto name = detail.has_value() ? detail->name : std::string_view{};
    const auto message = detail.has_value() ? detail->message : std::string_view{"http"};
    return exec::VenueError::make(classify(response.status, name), response.status, message);
}

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Named BEFORE the instrument table is read: InstrumentTable::find
// compares the whole id, so an id for a market we never loaded resolves to
// nothing and would otherwise be reported as an unknown instrument.
exec::VenueError build_failure(BuildError err) noexcept {
    switch (err) {
    case BuildError::DatedFuturesUnsupported:
        return exec::not_implemented("bitmex dated futures order entry");
    case BuildError::OptionsUnsupported:
        return exec::not_implemented("bitmex options order entry");
    case BuildError::NotSigned:
        // Misconfigured, not broken: the reconciler retries once an operator
        // fixes it, and halting the venue as Terminal is the wrong response.
        return exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, to_string(err));
    case BuildError::UnknownInstrument:
    case BuildError::BadQuantity:
    case BuildError::BadPrice:
    case BuildError::BufferTooSmall:
    case BuildError::NoOrderTarget:
    case BuildError::EmptyBatch:
    case BuildError::BatchTooLarge:
        break;
    }
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    return refusal.has_value() ? std::optional{build_failure(*refusal)} : std::nullopt;
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path builds its own requests and is answered asynchronously;
    // everything else here is a synchronous read.
    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        std::vector<exec::OrderStatusReport> reports;
        for (std::uint32_t page = 0; page < k_max_pages; ++page) {
            const auto body = pull(builder_.open_orders(page * SignedRequestBuilder::k_page_size,
                                                        state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto rows = parse_orders(*body, storage);
            if (!rows.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
            }
            for (const auto& wire : *rows) {
                // BitMEX offers no market parameter: one call answers for spot
                // and the perpetuals at once, so the caller's market is applied
                // here rather than pretended at the request.
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
            if (rows->size() < SignedRequestBuilder::k_page_size) {
                break;
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.query_order(id, state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto rows = parse_orders(*body, storage);
        if (!rows.has_value() || rows->empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitmex: no order with that client id")};
        }
        auto report = map_open_order(rows->front(), state_->instruments);
        if (!report.has_value()) {
            // The filter matched, so the order exists; it is simply no longer
            // working. Saying that beats reporting a finished order as live.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitmex: order is not in the working set")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        std::vector<exec::FillReport> reports;
        for (std::uint32_t page = 0; page < k_max_pages; ++page) {
            const auto body = pull(builder_.fills(
                since, page * SignedRequestBuilder::k_page_size, state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local ExecutionBuffer storage;
            const auto rows = parse_executions(*body, storage);
            if (!rows.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
            }
            for (const auto& wire : *rows) {
                auto report = map_fill(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
            if (rows->size() < SignedRequestBuilder::k_page_size) {
                break;
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.positions(state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local PositionBuffer storage;
        const auto rows = parse_positions(*body, storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            auto report = map_position(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    // 500 rows a page; four pages is more open orders than any account this
    // framework is aimed at holds, and an unbounded sweep on a cold path is how
    // a reconnect earns a rate-limit ban.
    static constexpr std::uint32_t k_max_pages = 4;

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(std::expected<net::RestRequest, BuildError>&& request) {
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 20'000);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status != k_ok) {
            return std::unexpected{rest_failure(*response)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::bitmex::detail
