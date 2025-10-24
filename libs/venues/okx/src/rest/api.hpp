#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what the account is long, what it was charged. None of them belongs to the
// order path — they run on a poller's schedule.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/catalogue.hpp>
#include <raikiri/venues/okx/endpoints.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/orders.hpp>
#include <raikiri/venues/okx/positions.hpp>
#include <raikiri/venues/okx/rest.hpp>

#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::okx::detail {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer} {
        builder_.set_passphrase(state.passphrase.view());
        builder_.set_simulated(state.testnet);
    }

    void start() {
        rest_.start();
        sync_clock(*state_, rest_);
    }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // Which markets this adapter will place orders in at all. Exhaustive so
    // that a new MarketType has to be answered here rather than inheriting
    // the swap path by accident.
    [[nodiscard]] static std::optional<exec::VenueError>
    order_entry_market(MarketType market) noexcept {
        switch (market) {
        case MarketType::Spot:
        case MarketType::Perp:
        case MarketType::Future:
            return std::nullopt;
        case MarketType::Option:
            return exec::not_implemented("okx options order entry");
        }
        return exec::not_implemented("okx order entry for an unknown market");
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(const std::expected<net::RestRequest, BuildError>& request) {
        if (!request.has_value()) {
            return std::unexpected{exec::VenueError::make(request.error() == BuildError::NotSigned
                                                              ? exec::ErrorClass::AuthFailure
                                                              : exec::ErrorClass::Terminal,
                                                          0,
                                                          to_string(request.error()))};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable envelope")};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return response->body;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.open_orders(market, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (const auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        // GET /trade/order needs an instId, and a client order id alone does
        // not name one. Scanning the pending list is the only lookup that
        // works from what the caller has.
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
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.fills(market, since, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FillBuffer storage;
        const auto fills = parse_fills(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            if (const auto report = map_fill(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) {
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.funding_book(market, since, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local BillBuffer storage;
        const auto bills = parse_bills(*body, storage);
        if (!bills.has_value()) {
            // A book we cannot read is NOT an empty one. Saying "no charges"
            // here books zero funding against a position that paid some.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bills.error()))};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(bills->size());
        for (const auto& wire : *bills) {
            // `8` is funding, `2` a trade. Everything else on this book —
            // transfers, interest, conversions — is not trading money, and a
            // row that names no instrument cannot be attributed at all.
            LedgerKind kind{};
            if (wire.type == "8") {
                kind = LedgerKind::Funding;
            } else if (wire.type == "2") {
                kind = LedgerKind::Fee;
            } else {
                continue;
            }
            const auto* instrument = state_->instruments.find(wire.inst_id);
            if (instrument == nullptr) {
                continue;
            }
            // THE COLUMN DEPENDS ON THE ROW: funding states its money in
            // balChg and leaves fee at zero, a trade the reverse.
            const auto text = kind == LedgerKind::Funding ? wire.bal_chg : wire.fee;
            const auto amount = parse_decimal_e9(text);
            if (!amount.has_value() || amount->raw == 0) {
                continue; // an unreadable or absent figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            // Both columns are already signed the way the account moved, so
            // neither is flipped — unlike bybit, which states a fee as a cost.
            charge.amount = *amount;
            charge.kind = kind;
            charge.settled_at = UnixNanos{wire.ts_ms * 1'000'000ULL};
            // A fee names the trade it was charged on, which is what the
            // comparison keys on; the book's own row id is the fallback.
            const auto reference =
                kind == LedgerKind::Fee && !wire.trade_id.empty() ? wire.trade_id : wire.bill_id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        switch (market) {
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Spot:
            // GET /account/positions does not accept instType SPOT: OKX holds
            // spot inventory as a per-currency balance, and one BTC balance
            // backs BTC-USDT, BTC-USDC and BTC-USD alike. Answering "flat"
            // would tell the reconciler we hold nothing.
            return std::unexpected{
                exec::not_implemented("okx spot positions: the venue reports balances, not "
                                      "positions")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("okx options order entry")};
        }
        const auto body = pull(builder_.positions(market, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local PositionBuffer storage;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (const auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        // ONE TRADING ACCOUNT behind both markets: OKX funds a spot fill and a
        // swap's margin out of the same currencies, so the answer does not
        // vary by market and narrowing it to one would be an invention.
        (void)market;
        // ASKED FIRST, and not fatal when it fails: the mode is what makes a
        // per-currency available figure readable, and a balance set without it
        // is still worth publishing with that one field undef.
        auto mode = AccountMode::Unknown;
        if (const auto config = pull(builder_.account_config(state_->venue_now()));
            config.has_value()) {
            if (const auto read = parse_account_mode(*config); read.has_value()) {
                mode = *read;
            }
        }
        const auto body = pull(builder_.balances(state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local BalanceBuffer storage;
        const auto rows = parse_balances(*body, storage);
        if (!rows.has_value()) {
            // An unreadable wallet is refused rather than reported as an empty
            // vector, which reads as an account holding nothing.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<BalanceReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto report = map_balance(wire, mode, state_->clock->wall());
                report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

} // namespace raikiri::venues::okx::detail
