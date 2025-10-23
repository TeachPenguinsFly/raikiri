#pragma once

// Every signed call this venue answers.
//
// The reads here are what a reconciliation asks: what is open, what filled,
// what the account is long. None of them belongs to the order path — they run
// on a poller's schedule.

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/orders.hpp>
#include <raikiri/venues/bingx/positions.hpp>
#include <raikiri/venues/bingx/rest.hpp>

#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bingx::detail {

// A build that failed for want of credentials is an AuthFailure, not a
// Terminal one: the gateway must halt and say so rather than retry a request
// that will fail identically forever.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Answered BEFORE the instrument table is read.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    if (!refusal.has_value()) {
        return std::nullopt;
    }
    return exec::not_implemented(to_string(*refusal));
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer} {}

    void start() {
        rest_.start();
        sync_clock(*state_, rest_, unsigned_builder_);
    }

    void stop() { rest_.stop(); }

    // A venue reaps a socket it has not heard from without saying so, and
    // finding out on an order costs a connect plus TLS at the worst moment.
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // The order path and the listen key build their own requests and are
    // answered asynchronously; everything else here is a synchronous read.
    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // The listen-key endpoints answer in HTTP statuses and no envelope at all,
    // so fetch() cannot read them.
    [[nodiscard]] std::optional<net::RestResponse> fetch_raw(const net::RestRequest& request) {
        return await_response(*state_, rest_, request);
    }

    // Hedge and one-way need different parameters and BingX refuses the wrong
    // pair with 109400, so this is read once at start rather than guessed per
    // order. A read that fails leaves one-way, which is BingX's own default.
    void read_position_mode() {
        const auto request = builder_.position_mode(state_->venue_now());
        if (!request.has_value()) {
            return;
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return;
        }
        if (const auto hedge = parse_position_mode(*body); hedge.has_value()) {
            builder_.set_hedge_mode(*hedge);
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
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
            auto report = map_open_order(wire, state_->instruments, market);
            if (!report.has_value()) {
                continue; // another market's pair, or an instrument we never loaded
            }
            state_->remember(wire.order_id, report->exchange_client_order_id);
            reports.push_back(*report);
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // Both single-order reads demand a SYMBOL and this call carries none,
        // so the open-order sweep answers the same question rather than an
        // endpoint that cannot hear it.
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
                                                                                UnixNanos) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // /openApi/spot/v1/trade/myTrades demands a symbol and BingX
            // offers no account-wide form; sweeping the pairs we happen to
            // have touched would answer "these are all of them" about a set
            // that is empty on every restart.
            return std::unexpected{
                exec::not_implemented("bingx spot fill pull (myTrades is per-symbol)")};
        }
        // /allFillOrders is account-wide but states no trade id and no side:
        // a FillReport without an exec id cannot be deduplicated and one
        // without a side cannot be netted. Live fills are unaffected —
        // ORDER_TRADE_UPDATE carries both.
        return std::unexpected{exec::not_implemented(
            "bingx perpetual fill pull (allFillOrders states no trade id and no side)")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
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
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // The spot tree states `free` and `locked` per asset and no total
            // of its own, so a total read there would be a sum this process
            // made up rather than a figure BingX stated.
            return std::unexpected{
                exec::not_implemented("bingx spot balance pull (the tree states no total)")};
        }
        const auto request = builder_.balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
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
        // This answer carries no stamp of its own, so the read's own time is
        // what the rows are dated by.
        const auto ts = state_->clock->wall();
        std::vector<BalanceReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto report = map_balance(wire, ts); report.has_value()) {
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
            // The book is a swap path. Spot has no funding to charge, and its
            // fees are already booked off the fill that incurred them.
            return std::unexpected{exec::not_implemented("bingx spot funding charges")};
        }
        const auto request = builder_.funding_income(since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local IncomeBuffer storage;
        const auto rows = parse_income(*body, storage);
        if (!rows.has_value()) {
            // A book we cannot read is NOT an empty one: answering "no
            // funding" books zero against a position that paid some.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            LedgerKind kind{};
            if (wire.income_type == "FUNDING_FEE") {
                kind = LedgerKind::Funding;
            } else if (wire.income_type == "TRADING_FEE") {
                kind = LedgerKind::Fee;
            } else {
                // REALIZED_PNL, the transfers and the liquidation kinds are
                // derived from fills this process already booked, so taking
                // the venue's copy would count the same money twice.
                continue;
            }
            const auto* instrument = state_->instruments.find(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            // BingX spells a spot pair and a perpetual IDENTICALLY, so the
            // by-symbol lookup answers with whichever market loaded first.
            // This book is a swap path, and a charge attributed to the spot
            // pair would be booked against a position that does not exist.
            const auto& base = base_of(*instrument);
            if (base.id.market() != MarketType::Perp) {
                continue;
            }
            // SIGNED AS SENT. BingX writes a payment negative and a receipt
            // positive, which is already the ledger's convention — taking the
            // magnitude here would report every funding payment as income.
            const auto amount = parse_decimal_e9(wire.income);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base.id;
            charge.amount = *amount;
            charge.kind = kind;
            charge.settled_at = UnixNanos{wire.time_ms * 1'000'000ULL};
            // A fee names the trade it was charged on, which is what the
            // comparison keys on; the book's own row id is the fallback.
            const auto reference =
                kind == LedgerKind::Fee && !wire.trade_id.empty() ? wire.trade_id : wire.tran_id;
            if (const auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

private:
    // A refusal arrives at HTTP 200 with the verdict in the body, so the status
    // is checked for transport health and the ENVELOPE decides the outcome.
    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "http error")};
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

    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder unsigned_builder_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::bingx::detail
