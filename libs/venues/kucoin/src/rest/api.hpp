#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::kucoin::internal {

// 100 rows a page covers 89 days of hourly settlements in 22 pages
inline constexpr std::size_t k_max_funding_pages = 24;
inline constexpr std::uint64_t k_max_closed_pages = 5;

class RestApi {
public:
    RestApi(VenueState& state, MarketType market)
        : state_{&state}, market_{market},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(market)}, .port = "443"}},
          builder_{state.credentials} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // orders are answered asynchronously; the reads here are synchronous
    [[nodiscard]] OrderRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // dated futures are served by the perpetual host
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
                                      market == MarketType::Spot ? "kucoin spot is not enabled on this gateway"
                                                                 : "kucoin futures is not enabled on this gateway");
    }

    // single-connection ticket from a signed post; an auth failure is final
    [[nodiscard]] std::expected<PrivateTicket, exec::VenueError> private_ticket() {
        const auto request = builder_.bullet_private(market_, state_->venue_now());
        if (!request.has_value()) {
            // no credentials: retrying cannot produce them
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, to_string(request.error()))};
        }
        const auto body = usable_body(await_response(*state_, rest_, *request, 10'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        // WireBullet views into body, so build the endpoint while body is alive
        const auto bullet = parse_bullet(*body);
        if (!bullet.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bullet.error()))};
        }
        auto endpoint = make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bullet names no usable endpoint")};
        }
        return PrivateTicket{.endpoint = std::move(*endpoint), .ping_interval_ms = bullet->ping_interval_ms};
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
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

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        // neither host reads by clientOid with this signature, so sweep actives
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
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
            // the spot wallet is a separate account on the spot host
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto balance = map_account_overview(*wire, state_->venue_now());
        if (!balance.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "kucoin: account overview names no currency")};
        }
        // one settle currency per gateway
        return std::vector<BalanceReport>{*balance};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
        switch (market) {
        case MarketType::Spot:
            // spot has no positions; an empty list would read as flat
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
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

    // funding history answers one symbol at a time, so ask for each symbol held now or closed since `since`
    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        switch (market) {
        case MarketType::Spot:
            return std::unexpected{exec::not_implemented("kucoin spot has no funding")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        if (!serves(market)) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto now = state_->venue_now();
        const auto from = UnixNanos{
            std::max(since.ns, now.ns > k_funding_window_ns ? now.ns - k_funding_window_ns : 0)};
        auto symbols = funded_symbols(from);
        if (!symbols.has_value()) {
            return std::unexpected{symbols.error()};
        }
        std::vector<LedgerEntry> charges;
        std::unordered_set<std::uint64_t> seen;
        for (const auto& symbol : *symbols) {
            std::uint64_t offset = 0;
            for (std::size_t page = 0; page < k_max_funding_pages; ++page) {
                const auto request = builder_.funding_history(symbol, from, offset, state_->venue_now());
                if (!request.has_value()) {
                    return std::unexpected{build_failed(request.error())};
                }
                const auto body = usable_body(await_response(*state_, rest_, *request));
                if (!body.has_value()) {
                    // a partial sweep fails whole
                    return std::unexpected{body.error()};
                }
                static thread_local const auto storage_owner = std::make_unique<FundingChargeBuffer>();
                const auto wire = parse_funding_history(*body, *storage_owner);
                if (!wire.has_value()) {
                    // an unreadable body is an error; an empty ledger would book zero funding
                    return std::unexpected{
                        exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
                }
                const bool fresh = collect_funding_page(
                    wire->charges, state_->instruments, state_->clock->wall(), seen, charges);
                if (!wire->has_more || !fresh) {
                    break;
                }
                offset = wire->charges.back().id;
            }
        }
        return charges;
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return collect(*wire);
    }

    // spot actives are per symbol, so list the symbols first
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
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(symbols.error()))};
        }

        std::vector<exec::OrderStatusReport> reports;
        for (const auto symbol : *symbols) {
            const auto request = builder_.open_orders(MarketType::Spot, symbol, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = usable_body(await_response(*state_, rest_, *request));
            if (!body.has_value()) {
                // a partial sweep fails whole
                return std::unexpected{body.error()};
            }
            static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
            auto& storage = *storage_owner;
            const auto wire = parse_spot_orders(*body, storage);
            if (!wire.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& row : *wire) {
                if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    // a charge at t on a symbol means a position open at t: still open now, or closed after t
    std::expected<std::vector<std::string>, exec::VenueError> funded_symbols(UnixNanos from) {
        std::vector<std::string> symbols;
        const auto remember = [&symbols](std::string_view symbol) {
            if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
                symbols.emplace_back(symbol);
            }
        };
        const auto open = builder_.positions(market_, state_->venue_now());
        if (!open.has_value()) {
            return std::unexpected{build_failed(open.error())};
        }
        const auto open_body = usable_body(await_response(*state_, rest_, *open));
        if (!open_body.has_value()) {
            return std::unexpected{open_body.error()};
        }
        static thread_local const auto open_owner = std::make_unique<PositionBuffer>();
        const auto held = parse_positions(*open_body, *open_owner);
        if (!held.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(held.error()))};
        }
        for (const auto& row : *held) {
            remember(row.symbol);
        }
        for (std::uint64_t page = 1; page <= k_max_closed_pages; ++page) {
            const auto request = builder_.closed_positions(from, page, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = usable_body(await_response(*state_, rest_, *request));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local const auto closed_owner = std::make_unique<ClosedPositionBuffer>();
            const auto closed = parse_history_positions(*body, *closed_owner);
            if (!closed.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(closed.error()))};
            }
            for (const auto& row : closed->positions) {
                remember(row.symbol);
            }
            if (page >= closed->total_pages || closed->positions.empty()) {
                break;
            }
        }
        return symbols;
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
