#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/orders.hpp>
#include <raikiri/venues/htx/positions.hpp>
#include <raikiri/venues/htx/rest.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::htx::internal {

// the code travels in the message since spot spells it as a word
exec::VenueError rest_failure(std::string_view body) {
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    if (envelope->ok) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "refusal body reports ok");
    }
    return exec::VenueError::make(classify(envelope->code), 0, envelope->msg);
}

exec::VenueError build_failure(BuildError err) noexcept {
    // a build refusal is terminal: rebuilding gives the same result
    return exec::VenueError::make(
        err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal, 0, to_string(err));
}

class RestApi {
public:
    RestApi(VenueState& state, MarketType market)
        : state_{&state},
          perp_rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .pool_size = net::k_order_pool_size}},
          spot_rest_{state.io,
                     net::RestConfig{.host = std::string{spot_rest_host()}, .pool_size = net::k_order_pool_size}},
          builder_{state.signer}, spot_enabled_{market == MarketType::Spot} {}

    std::expected<void, exec::VenueError> start() {
        perp_rest_.start();
        spot_rest_.start();
        sync_clock(*state_, perp_rest_);
        if (spot_enabled_ && !load_spot_account()) {
            // without the account id every spot order is refused
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "htx: spot account id unavailable")};
        }
        return {};
    }

    void stop() {
        perp_rest_.stop();
        spot_rest_.stop();
    }

    // keeps pooled connections alive so an order does not pay a reconnect
    void keepalive(UnixNanos now) noexcept {
        perp_rest_.keepalive(now);
        spot_rest_.keepalive(now);
    }

    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, MarketType market, net::RestCallback on_done) {
        return rest_for(market).send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, 1, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();

        auto& storage = *storage_owner;
        const auto orders = parse_rest_orders(*body, storage, market);
        if (!orders.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            state_->remember(wire);
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        std::int64_t numeric = 0;
        std::string_view contract;
        if (market != MarketType::Spot) {
            const auto it = state_->swap_targets.find(std::string{id.view()});
            if (it == state_->swap_targets.end()) {
                // the swap never saw this ulid; the pairing lives only in this process
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "htx swap: client order id not minted here")};
            }
            numeric = it->second.client_order_id;
            contract = it->second.contract_code;
        }
        const auto request = builder_.query_order(id, numeric, contract, market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();

        auto& storage = *storage_owner;
        const auto orders = parse_rest_orders(*body, storage, market);
        if (!orders.has_value() || orders->empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not readable")};
        }
        auto report = map_open_order(orders->front(), state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos since) {
        (void)since;
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // trade history is per contract on both surfaces, and an empty vector would read as no fills
        return std::unexpected{exec::not_implemented("htx fill pull is per-contract")};
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot pays no funding and this ledger is the swap account's
            return std::unexpected{exec::not_implemented("htx spot funding charges")};
        }
        const auto request = builder_.financial_record(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FinancialRecordBuffer>();

        auto& storage = *storage_owner;
        const auto rows = parse_financial_records(*body, storage);
        if (!rows.has_value()) {
            // an unreadable ledger is not an empty one; v3 refusals say code instead of status and land here
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto charge = map_financial_record(wire, state_->instruments, received); charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // a spot balance is not a position
            return std::unexpected{exec::not_implemented("htx spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();

        auto& storage = *storage_owner;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
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
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& request,
                                                                     MarketType market) {
        const auto response = await_response(*state_, rest_for(market), request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{
                exec::VenueError::make(classify_http(response->status), response->status, "http error")};
        }
        // htx refuses with http 200 and status "error"
        const auto envelope = parse_rest_envelope(response->body);
        if (envelope.has_value() && !envelope->ok) {
            return std::unexpected{rest_failure(response->body)};
        }
        return response->body;
    }

    [[nodiscard]] net::RestClient& rest_for(MarketType market) noexcept {
        return market == MarketType::Spot ? spot_rest_ : perp_rest_;
    }

    [[nodiscard]] bool load_spot_account() {
        if (builder_.spot_account_id() != 0) {
            return true;
        }
        const auto request = builder_.spot_accounts(state_->venue_now());
        if (!request.has_value()) {
            return false;
        }
        const auto body = fetch(*request, MarketType::Spot);
        if (!body.has_value()) {
            return false;
        }
        const auto id = parse_spot_account_id(*body);
        if (!id.has_value() || *id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        builder_.set_spot_account_id(static_cast<std::int64_t>(*id));
        return true;
    }

    VenueState* state_;
    net::RestClient perp_rest_;
    net::RestClient spot_rest_;
    SignedRequestBuilder builder_;
    bool spot_enabled_{false};
};

} // namespace raikiri::venues::htx::internal
