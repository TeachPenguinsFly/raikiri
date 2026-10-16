#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::hyperliquid::internal {

// checked before the table lookup, so an unserved market is not reported as UnknownInstrument
inline std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("hyperliquid dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("hyperliquid options order entry");
    }
    return exec::not_implemented("hyperliquid: unknown market");
}

// the account is never defaulted to the signer's address, since an
// api wallet signs for another account
inline exec::VenueError no_address() noexcept {
    return exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "hyperliquid: no account address configured");
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}},
          builder_{state.wallet, !state.testnet} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // sweep rest connections the venue closed so an idle gateway does not write into a dead socket
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] ExchangeBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        static thread_local const auto storage_owner = std::make_unique<OpenOrderBuffer>();

        auto& storage = *storage_owner;
        std::vector<exec::OrderStatusReport> reports;
        // the main dex's answer carries spot too; each hip-3 dex answers for its own orders only
        for (std::size_t d = 0; d <= state_->loaded_dexes.size(); ++d) {
            const std::string_view dex = d == 0 ? std::string_view{} : state_->loaded_dexes[d - 1];
            // frontendOpenOrders carries `isTrigger`, which openOrders omits
            const auto body = read_info(*state_, rest_, requests_.frontend_open_orders(state_->address, dex));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto orders = parse_open_orders(*body, storage);
            if (!orders.has_value()) {
                return std::unexpected{unreadable(orders.error())};
            }
            for (const auto& wire : *orders) {
                // one answer carries both markets, so filter by the instrument's market
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
            if (market == MarketType::Spot) {
                break;
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        std::array<char, k_cloid_chars> buf{};
        const auto cloid = cloid_text(id, buf);
        if (cloid.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "hyperliquid: client order id is not a ULID")};
        }
        const auto body = read_info(*state_, rest_, requests_.order_status(state_->address, cloid));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        const auto status = parse_order_status(*body);
        if (!status.has_value()) {
            return std::unexpected{unreadable(status.error())};
        }
        if (!status->found) {
            // "unknownOid" covers both forgotten and never-minted ids
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "hyperliquid: order unknown to the venue")};
        }
        const auto report = map_order_status(*status, state_->instruments);
        if (!report.has_value() || report->instrument.market() != market) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // `startTime` is inclusive ms, and zero does not mean from the beginning
        const auto since_ms = since.ns / 1'000'000ULL;
        const auto request = since_ms == 0 ? requests_.user_fills(state_->address)
                                           : requests_.user_fills_by_time(state_->address, since_ms);
        const auto body = read_info(*state_, rest_, request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FillBuffer>();

        auto& storage = *storage_owner;
        const auto fills = parse_fills(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{unreadable(fills.error())};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            auto report = map_fill(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<FeeRates, exec::VenueError> fee_rates() {
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        const auto body = read_info(*state_, rest_, requests_.user_fees(state_->address));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_user_fees(*body);
        if (!wire.has_value()) {
            return std::unexpected{unreadable(wire.error())};
        }
        const auto rates = map_user_fees(*wire);
        if (!rates.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "hyperliquid: fee rates unreadable")};
        }
        return *rates;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot pays no funding; this ledger is perp-only
            return std::unexpected{exec::not_implemented("hyperliquid spot funding charges")};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // an inclusive start returns the cursor's row again and the caller
        // drops it by stamp; 500 rows per page
        const auto body = read_info(*state_, rest_, requests_.user_funding(state_->address, since.ns / 1'000'000ULL));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local const auto storage_owner = std::make_unique<FundingChargeBuffer>();

        auto& storage = *storage_owner;
        const auto rows = parse_user_funding(*body, storage);
        if (!rows.has_value()) {
            // an unreadable ledger must not read as no funding
            return std::unexpected{unreadable(rows.error())};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto charge = map_funding_charge(wire, state_->instruments, received); charge.has_value()) {
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
            // spotClearinghouseState returns balances with no entry price, so spot has no positions
            return std::unexpected{exec::not_implemented("hyperliquid spot position pull")};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        static thread_local const auto storage_owner = std::make_unique<PositionBuffer>();

        auto& storage = *storage_owner;
        std::vector<exec::PositionReport> reports;
        for (std::size_t d = 0; d <= state_->loaded_dexes.size(); ++d) {
            const std::string_view dex = d == 0 ? std::string_view{} : state_->loaded_dexes[d - 1];
            const auto body = read_info(*state_, rest_, requests_.clearinghouse_state(state_->address, dex));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto positions = parse_positions(*body, storage);
            if (!positions.has_value()) {
                return std::unexpected{unreadable(positions.error())};
            }
            const auto ts = ms_to_unix(storage.ts_ms, state_->clock->wall());
            for (const auto& wire : *positions) {
                if (auto report = map_position(wire, state_->instruments, ts); report.has_value()) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

private:
    VenueState* state_;
    net::RestClient rest_;
    UserRequestBuilder requests_;
    ExchangeBuilder builder_;
};

} // namespace raikiri::venues::hyperliquid::internal
