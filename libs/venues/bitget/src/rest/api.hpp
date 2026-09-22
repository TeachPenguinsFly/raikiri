#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held. None of them belongs to the order path — they run on a
// poller's schedule and pump the reactor until the answer arrives, which an
// order may never do.

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
        // The passphrase view is taken here, not in the constructor: it
        // reflects the secret's length at the moment it is read, and start()
        // is the first point at which every credential is certainly resolved.
        builder_.set_credentials(state_->signer, state_->passphrase.view());
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
            // A spot pair pays no funding, and /api/v2/spot/account/bills is
            // another row shape carrying none. Answering with the mix ledger
            // would book a perpetual's charges against a spot gateway.
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
            // A ledger we cannot read is NOT an empty one: answering "no
            // funding" would book zero against a position that paid some.
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
            // Spot holds coin balances, not per-instrument positions: there is
            // nothing to report against an InstrumentId, and answering "flat"
            // would tell a reconciler the account is empty.
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

    // Both order pulls answer with the same rows under different envelopes,
    // so the market chooses the reader and names its own refusal in one place.
    //
    // `body` is the CALLER's: every returned view points into it, so a body
    // owned here would be freed before the first row was read.
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
            // Delivery contracts never reach the instrument table, so every
            // row would map to nothing and the pull would read as "nothing open".
            return std::unexpected{exec::not_implemented("bitget delivery futures order pull")};
        case MarketType::Option:
            // Bitget v2 lists no crypto options; its only options product is
            // US equity options behind the separate v3 UTA API.
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
