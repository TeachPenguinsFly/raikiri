#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/venues/bitget/rest.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bitget::internal {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    void start() {
        // read the passphrase at start, once every credential is resolved
        builder_.set_credentials(state_->signer, state_->passphrase.view());
        rest_.start();
        sync_clock(*state_, rest_);
    }

    void stop() { rest_.stop(); }

    // sweeps the rest pool so an idle gateway never writes an order into a dead socket
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows =
            pull_orders(builder_.open_orders(market, state_->product(), state_->venue_now()),
                        market,
                        *storage,
                        body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows = pull_orders(
            builder_.order_by_client_id(id, market, state_->product(), state_->venue_now()),
            market,
            *storage,
            body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        for (const auto& wire : *rows) {
            if (wire.client_oid != id.view()) {
                continue;
            }
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                return *report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        using Reader =
            std::expected<std::span<const WireFill>, WireError> (*)(std::string_view, FillBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_fills;
            break;
        case MarketType::Perp:
            read = &parse_mix_fills;
            break;
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures fill pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request = builder_.fills(market, state_->product(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<FillBuffer>();
        const auto rows = read(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_fill_report(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            // spot pays no funding and its bills ledger has no funding rows
            return std::unexpected{exec::not_implemented("bitget spot funding charges")};
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures funding pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request =
            builder_.settle_bills(market, state_->product(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<BillBuffer>();
        const auto rows = parse_mix_bills(*body, *storage);
        if (!rows.has_value()) {
            // an unreadable ledger is not an empty one
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto charge = map_bill(wire, state_->instruments, received);
                charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            // spot holds coin balances, so there are no per-instrument positions
            return std::unexpected{exec::not_implemented("bitget spot position pull")};
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures position pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request = builder_.positions(market, state_->product(), state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    static constexpr std::uint64_t k_pull_timeout_ms = 10'000;

    // every returned view points into the caller's body
    std::expected<std::span<const WireOrderUpdate>, exec::VenueError>
    pull_orders(const std::expected<net::RestRequest, BuildError>& request,
                MarketType market,
                OrderUpdateBuffer& storage,
                std::string& body) {
        using Reader = std::expected<std::span<const WireOrderUpdate>, WireError> (*)(
            std::string_view, OrderUpdateBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_orders;
            break;
        case MarketType::Perp:
            read = &parse_mix_orders;
            break;
        case MarketType::Future:
            // delivery contracts never reach the instrument table
            return std::unexpected{exec::not_implemented("bitget delivery futures order pull")};
        case MarketType::Option:
            // bitget v2 has no crypto options
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        auto fetched = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!fetched.has_value()) {
            return std::unexpected{fetched.error()};
        }
        body = std::move(*fetched);
        const auto rows = read(body, storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        return *rows;
    }

    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

} // namespace raikiri::venues::bitget::internal
