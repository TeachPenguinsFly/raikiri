#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held, what the wallet carries. None of them belongs to the order
// path — they run on a poller's schedule. The bullet token the private socket
// opens on is minted here too, because minting it is a signed REST call and
// nothing else about the socket is.
//
// One market family: one host, one signed pool, one builder. Spot and futures
// share nothing but the signing scheme, so nothing is shared here either —
// not the connection, not the paths, not the size units.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::kucoin::internal {

class RestApi {
public:
    RestApi(VenueState& state, MarketType market)
        : state_{&state}, market_{market},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(market)}, .port = "443"}},
          builder_{state.credentials} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path builds its own requests and is answered asynchronously;
    // everything else here is a synchronous read.
    [[nodiscard]] OrderRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // Whether an order or a read for this market can be answered at all. A
    // dated future is served by the perpetual host, and nothing else is.
    [[nodiscard]] bool serves(MarketType market) const noexcept {
        const bool futures = market == MarketType::Perp || market == MarketType::Future;
        return market_ == market || (futures && market_ == MarketType::Perp);
    }

    [[nodiscard]] static exec::VenueError build_failed(BuildError err) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
    }

    [[nodiscard]] static exec::VenueError market_not_enabled(MarketType market) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal,
                                      0,
                                      market == MarketType::Spot
                                          ? "kucoin spot is not enabled on this gateway"
                                          : "kucoin futures is not enabled on this gateway");
    }

    // The private socket's ticket, minted by a SIGNED POST and good for one
    // connection. An AuthFailure here is final — retrying a refused key at
    // full speed is how an account earns a lockout — and every other failure
    // is worth another attempt.
    [[nodiscard]] std::expected<PrivateTicket, exec::VenueError> private_ticket() {
        const auto request = builder_.bullet_private(market_, state_->venue_now());
        if (!request.has_value()) {
            // No credentials: retrying cannot produce them.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, to_string(request.error()))};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request, 10'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        // `WireBullet`'s fields are VIEWS INTO `body`, so the endpoint — which
        // owns its strings — is built here, while the body is still alive.
        const auto bullet = parse_bullet(*body);
        if (!bullet.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bullet.error()))};
        }
        auto endpoint = make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bullet names no usable endpoint")};
        }
        return PrivateTicket{.endpoint = std::move(*endpoint),
                             .ping_interval_ms = bullet->ping_interval_ms};
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        switch (market) {
        case MarketType::Spot:
            if (!serves(market)) {
                return std::unexpected{market_not_enabled(market)};
            }
            return spot_open_orders();
        case MarketType::Perp:
        case MarketType::Future:
            if (!serves(market)) {
                return std::unexpected{market_not_enabled(market)};
            }
            return futures_open_orders();
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        return std::unexpected{exec::not_implemented("kucoin options order entry")};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        // Neither host can answer this directly: futures offers no read by
        // clientOid at all, and the spot one demands a symbol this signature
        // does not carry. The active sweep is the only route to the answer.
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
        switch (market) {
        case MarketType::Spot:
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        if (!serves(market)) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = builder_.fills(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<FillBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_fills(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_fill(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        switch (market) {
        case MarketType::Spot:
            // KuCoin does have a spot wallet, GET /api/v1/accounts on the other
            // host. Refused rather than answered from the futures overview:
            // they are two accounts, and money moves between them by transfer.
            return std::unexpected{exec::not_implemented("kucoin spot balance pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        if (!serves(market)) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = builder_.account_overview(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_account_overview(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto balance = map_account_overview(*wire, state_->venue_now());
        if (!balance.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: account overview names no currency")};
        }
        // ONE ROW: the overview covers one settle currency and this gateway
        // trades one. A second settle currency is a second wallet and a second
        // process, exactly as a second market is.
        return std::vector<BalanceReport>{*balance};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        switch (market) {
        case MarketType::Spot:
            // Spot holds balances, not positions. An empty list would be read
            // as "flat", which is a claim about a concept this market lacks.
            return std::unexpected{exec::not_implemented("kucoin spot position pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        if (!serves(market)) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_positions(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_position(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> futures_open_orders() {
        const auto request = builder_.open_orders(market_, {}, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_futures_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return collect(*wire);
    }

    // KuCoin answers spot actives one symbol at a time, so the sweep asks which
    // symbols have any before asking for them. Two round trips, but the
    // alternative is a per-instrument loop over the whole catalogue.
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> spot_open_orders() {
        const auto listing = builder_.active_symbols(MarketType::Spot, state_->venue_now());
        if (!listing.has_value()) {
            return std::unexpected{build_failed(listing.error())};
        }
        const auto symbols_body = usable_body(await_response(*state_, rest_, *listing));
        if (!symbols_body.has_value()) {
            return std::unexpected{symbols_body.error()};
        }
        static thread_local const auto symbol_storage_owner = std::make_unique<SymbolBuffer>();
        auto& symbol_storage = *symbol_storage_owner;
        const auto symbols = parse_active_symbols(*symbols_body, symbol_storage);
        if (!symbols.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(symbols.error()))};
        }

        std::vector<exec::OrderStatusReport> reports;
        for (const auto symbol : *symbols) {
            const auto request =
                builder_.open_orders(MarketType::Spot, symbol, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = usable_body(await_response(*state_, rest_, *request));
            if (!body.has_value()) {
                // A partial sweep read as a complete one is a book that looks
                // emptier than it is, so the whole answer fails together.
                return std::unexpected{body.error()};
            }
            static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
            auto& storage = *storage_owner;
            const auto wire = parse_spot_orders(*body, storage);
            if (!wire.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& row : *wire) {
                if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::vector<exec::OrderStatusReport> collect(std::span<const WireOrder> rows) const {
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows.size());
        for (const auto& row : rows) {
            if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    VenueState* state_;
    MarketType market_;
    net::RestClient rest_;
    OrderRequestBuilder builder_;
};

} // namespace raikiri::venues::kucoin::internal
