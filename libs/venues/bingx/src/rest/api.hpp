#pragma once

#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/orders.hpp>
#include <raikiri/venues/bingx/positions.hpp>
#include <raikiri/venues/bingx/rest.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::bingx::internal {

// missing credentials are an AuthFailure so the gateway halts
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(
        err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal, 0, to_string(err));
}

// exhaustive over markets; checked before the instrument table
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
          rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .pool_size = net::k_order_pool_size}},
          builder_{state.signer} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // venues reap idle sockets silently
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] SignedRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // listen-key endpoints answer in http statuses with no envelope
    [[nodiscard]] std::optional<net::RestResponse> fetch_raw(const net::RestRequest& request) {
        return await_response(*state_, rest_, request);
    }

    // read once at start; a failed read leaves one-way, bingx's default
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
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

        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();

        auto& storage = *storage_owner;
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
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

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        // single-order reads need a symbol, so the open-order sweep answers instead
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

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market, UnixNanos) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot myTrades needs a symbol and has no account-wide form
            return std::unexpected{exec::not_implemented("bingx spot fill pull (myTrades is per-symbol)")};
        }
        // allFillOrders has no trade id or side, so its fills cannot be deduplicated or netted
        return std::unexpected{
            exec::not_implemented("bingx perpetual fill pull (allFillOrders states no trade id and no side)")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
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

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // spot states free and locked without a total
            return std::unexpected{exec::not_implemented("bingx spot balance pull (the tree states no total)")};
        }
        const auto request = builder_.balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BalanceBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_balances(*body, storage);
        if (!rows.has_value()) {
            // an unreadable wallet is refused so it does not read as empty
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        // the answer has no timestamp, so rows take the read time
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

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // funding is perp only
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
        static thread_local const auto storage_owner = std::make_unique<IncomeBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_income(*body, storage);
        if (!rows.has_value()) {
            // an unreadable book is refused so it does not read as zero funding
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
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
                // these kinds derive from fills already booked
                continue;
            }
            const auto* instrument = state_->instruments.find(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            // spot and perp share symbols; this book is perp only
            const auto& base = base_of(*instrument);
            if (base.id.market() != MarketType::Perp) {
                continue;
            }
            // bingx signs income as the ledger does; keep the sign
            const auto amount = parse_decimal_e9(wire.income);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base.id;
            charge.amount = *amount;
            charge.kind = kind;
            charge.settled_at = UnixNanos{wire.time_ms * 1'000'000ULL};
            // fees key on the trade id, falling back to the row id
            const auto reference = kind == LedgerKind::Fee && !wire.trade_id.empty() ? wire.trade_id : wire.tran_id;
            if (const auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

private:
    // refusals arrive at http 200; the envelope decides
    [[nodiscard]] std::expected<std::string, exec::VenueError> fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{
                exec::VenueError::make(classify_http(response->status), response->status, "http error")};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable envelope")};
        }
        if (envelope->code != k_ok) {
            return std::unexpected{exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
};

} // namespace raikiri::venues::bingx::internal
