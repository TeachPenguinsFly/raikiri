#pragma once

// Every call this venue answers, over the two endpoints it has: the unsigned
// /info reads and the signed /exchange write the order path builds through
// `builder()`.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what is held. None of them belongs to the order path — they run on a
// poller's schedule and pump the reactor until the answer arrives, which an
// order may never do.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::hyperliquid::detail {

// Asked BEFORE the instrument table is read: find() compares the WHOLE id, so
// an id for a market never loaded here resolves to nothing and would answer
// UnknownInstrument, blaming the catalogue for a market never served. Option is
// a named case so the compiler forces a decision rather than a default arm.
inline std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // The venue lists nothing dated: there are two catalogues, and they
        // are `meta.universe` (perpetuals) and `spotMeta.universe` (pairs).
        return exec::not_implemented("hyperliquid dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("hyperliquid options order entry");
    }
    return exec::not_implemented("hyperliquid: unknown market");
}

// The account is asked for explicitly and never defaulted to the signing key's
// own address: an approved API wallet signs for a DIFFERENT account by design,
// so guessing would reconcile a wallet holding nothing while trading the one
// that holds everything.
inline exec::VenueError no_address() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::AuthFailure, 0, "hyperliquid: no account address configured");
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io,
                net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}},
          builder_{state.wallet, !state.testnet} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path signs its own actions and is answered asynchronously;
    // everything else here is a synchronous read.
    [[nodiscard]] ExchangeBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // frontendOpenOrders, not openOrders: the plain form omits `isTrigger`,
        // so a dormant conditional reads there as a limit at its trigger price.
        const auto body =
            read_info(*state_, rest_, requests_.frontend_open_orders(state_->address));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OpenOrderBuffer storage;
        const auto orders = parse_open_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{unreadable(orders.error())};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            // One answer carries both markets — "BTC" beside "@107" — so the
            // filter is the instrument's own market, not a second request.
            auto report = map_open_order(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        std::array<char, k_cloid_chars> buf{};
        const auto cloid = cloid_text(id, buf);
        if (cloid.empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: client order id is not a ULID")};
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
            // "unknownOid" is not an error and not a status: an id the venue
            // has forgotten and one it never minted answer identically.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: order unknown to the venue")};
        }
        const auto report = map_order_status(*status, state_->instruments);
        if (!report.has_value() || report->instrument.market() != market) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // `startTime` is inclusive milliseconds, and zero is a different
        // request rather than "from the beginning".
        const auto since_ms = since.ns / 1'000'000ULL;
        const auto request = since_ms == 0
                                 ? requests_.user_fills(state_->address)
                                 : requests_.user_fills_by_time(state_->address, since_ms);
        const auto body = read_info(*state_, rest_, request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FillBuffer storage;
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

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot pair pays no funding, and this ledger holds perpetual
            // settlements alone. Answering with it would book a perp's charges
            // against a spot gateway.
            return std::unexpected{exec::not_implemented("hyperliquid spot funding charges")};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // `startTime` is INCLUSIVE, so the settlement the cursor sits on comes
        // back once more; the caller drops it by stamp. A zero start asks from
        // the account's first settlement, and the venue caps the answer at 500
        // rows — the cursor walks forward one page per pass rather than
        // skipping to the newest.
        const auto body = read_info(
            *state_, rest_, requests_.user_funding(state_->address, since.ns / 1'000'000ULL));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FundingChargeBuffer storage;
        const auto rows = parse_user_funding(*body, storage);
        if (!rows.has_value()) {
            // A ledger we cannot read is NOT an empty one: answering "no
            // funding" would book zero against a position that paid some.
            return std::unexpected{unreadable(rows.error())};
        }
        const auto received = state_->clock->wall();
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto charge = map_funding_charge(wire, state_->instruments, received);
                charge.has_value()) {
                charges.push_back(*charge);
            }
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // Before the address check: this is a fact about the VENUE, true
            // however the gateway is configured. spotClearinghouseState answers
            // BALANCES, and a balance is not a position — one HYPE holding
            // backs @107 and every other pair quoting it, with no entry price
            // anywhere in the response.
            return std::unexpected{exec::not_implemented("hyperliquid spot position pull")};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        const auto body = read_info(*state_, rest_, requests_.clearinghouse_state(state_->address));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local PositionBuffer storage;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{unreadable(positions.error())};
        }
        // One `time` for the whole snapshot; there is no per-row timestamp.
        const auto ts = ms_to_unix(storage.ts_ms, state_->clock->wall());
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_position(wire, state_->instruments, ts); report.has_value()) {
                reports.push_back(*report);
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

} // namespace raikiri::venues::hyperliquid::detail
