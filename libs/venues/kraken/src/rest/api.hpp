#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what the account is long. None of them belongs to the order path — they run
// on a poller's schedule.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/orders.hpp>
#include <raikiri/venues/kraken/positions.hpp>
#include <raikiri/venues/kraken/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::kraken::detail {

// /fills answers at most 100 rows and pages BACKWARDS from `lastFillTime`, so
// a `since` older than this many pages is answered with what we could reach
// rather than with an unbounded sweep of the account's history.
constexpr std::size_t k_max_fill_pages = 8;

// What /fills answers in one call; a shorter page is the end of the history.
constexpr std::size_t k_fills_per_page = 100;

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Named BEFORE the instrument table is read: InstrumentTable::find
// compares the whole id, so an id for a market we never loaded resolves to
// nothing and would otherwise be reported as an unknown instrument.
exec::VenueError refuse_market_error(BuildError err) noexcept {
    switch (err) {
    case BuildError::SpotUnsupported:
        // Kraken Spot is api.kraken.com: another host, another symbology and
        // another signature. This adapter never loaded it and cannot sign for
        // it, so it says so instead of blaming a stale catalogue.
        return exec::not_implemented("kraken spot order entry");
    case BuildError::DatedFuturesUnsupported:
        return exec::not_implemented("kraken dated futures order entry");
    case BuildError::OptionsUnsupported:
        return exec::not_implemented("kraken options order entry");
    case BuildError::UnknownInstrument:
    case BuildError::BadQuantity:
    case BuildError::BadPrice:
    case BuildError::BufferTooSmall:
    case BuildError::NotSigned:
    case BuildError::NoOrderTarget:
        break;
    }
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    return refusal.has_value() ? std::optional{refuse_market_error(*refusal)} : std::nullopt;
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. Missing credentials are the exception the
// reconciler acts on differently, which is why they keep their own class.
exec::VenueError build_failure(BuildError err) noexcept {
    if (err == BuildError::NotSigned) {
        return exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, to_string(err));
    }
    return refuse_market_error(err);
}

exec::VenueError rest_failure(std::string_view body) {
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    if (envelope->ok) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "refusal body reports ok");
    }
    return exec::VenueError::make(classify(envelope->error), 0, envelope->error);
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

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
        const auto body = pull(builder_.open_orders(state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OpenOrderBuffer>();
        auto& storage = *storage_owner;
        const auto orders = parse_open_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
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

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        // There is no single-order read in this API: /openorders is the whole
        // working set, filtered here. An order that has finished is therefore
        // not findable, and saying so beats reporting it as unknown.
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id == id) {
                return report;
            }
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "kraken: order is not in the working set")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto since_ms = since.ns / 1'000'000ULL;
        std::vector<exec::FillReport> reports;
        std::string cursor; // empty asks for the newest page
        for (std::size_t page = 0; page < k_max_fill_pages; ++page) {
            const auto body = pull(builder_.fills(cursor, state_->venue_now()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local const auto storage_owner = std::make_unique<FillBuffer>();
            auto& storage = *storage_owner;
            const auto fills = parse_fills(*body, storage);
            if (!fills.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
            }
            if (fills->empty()) {
                break;
            }
            // Rows come back newest first, so the last one carries the cursor
            // for the page before this one.
            std::uint64_t oldest_ms = 0;
            std::string_view oldest_text;
            bool reached_since = false;
            for (const auto& wire : *fills) {
                if (oldest_ms == 0 || (wire.fill_ms != 0 && wire.fill_ms < oldest_ms)) {
                    oldest_ms = wire.fill_ms;
                    oldest_text = wire.fill_time;
                }
                if (wire.fill_ms < since_ms) {
                    reached_since = true;
                    continue;
                }
                if (auto report = map_rest_fill(wire, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
            // A short page is the beginning of the account's history: the
            // endpoint answers "the 100 entries before lastFillTime", so fewer
            // than that means there is nothing older to ask for.
            if (reached_since || oldest_text.empty() || fills->size() < k_fills_per_page) {
                break;
            }
            cursor.assign(oldest_text);
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.positions(state_->venue_now()));
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
    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(std::expected<net::RestRequest, BuildError>&& request) {
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        // HTTP status is no substitute for the body: an unauthenticated read
        // answers HTTP 200 with "authenticationError".
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value() || !envelope->ok) {
            return std::unexpected{rest_failure(response->body)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::kraken::detail
