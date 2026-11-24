#pragma once

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

#include <memory>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::okx::internal {

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state}, rest_{state.io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state.signer} {
        builder_.set_passphrase(state.passphrase.view());
        builder_.set_simulated(state.testnet);
        builder_.set_trade_modes(modes_);
    }

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // keeps pooled rest connections alive between orders
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    [[nodiscard]] RequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // exhaustive so a new MarketType cannot inherit the swap path
    [[nodiscard]] static std::optional<exec::VenueError> order_entry_market(MarketType market) noexcept {
        switch (market) {
        case MarketType::Spot:
        case MarketType::Perp:
        case MarketType::Future:
        case MarketType::Option:
            return std::nullopt;
        }
        return exec::not_implemented("okx order entry for an unknown market");
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(const std::expected<net::RestRequest, BuildError>& request) {
        if (!request.has_value()) {
            return std::unexpected{exec::VenueError::make(
                request.error() == BuildError::NotSigned ? exec::ErrorClass::AuthFailure : exec::ErrorClass::Terminal,
                0,
                to_string(request.error()))};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType market) {
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.open_orders(market, state_->venue_now()));
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
            if (const auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) {
        // GET /trade/order needs an instId, so the pending list is scanned
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
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.fills(market, since, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<FillBuffer>();
        auto& storage = *storage_owner;
        const auto fills = parse_fills(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
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

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market, UnixNanos since) {
        if (const auto refusal = order_entry_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.funding_book(market, since, state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BillBuffer>();
        auto& storage = *storage_owner;
        const auto bills = parse_bills(*body, storage);
        if (!bills.has_value()) {
            // an unreadable bill book is not an empty one
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bills.error()))};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(bills->size());
        for (const auto& wire : *bills) {
            // 8 is funding, 2 a trade; other rows are not trading money
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
            // funding states its money in balChg, a trade in fee
            const auto text = kind == LedgerKind::Funding ? wire.bal_chg : wire.fee;
            const auto amount = parse_decimal_e9(text);
            if (!amount.has_value() || amount->raw == 0) {
                continue; // an unreadable or absent figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            // both columns are already signed the way the account moved
            charge.amount = *amount;
            charge.kind = kind;
            charge.settled_at = UnixNanos{wire.ts_ms * 1'000'000ULL};
            // a fee keys on its trade id, else on the bill id
            const auto reference = kind == LedgerKind::Fee && !wire.trade_id.empty() ? wire.trade_id : wire.bill_id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    // the mode is the one this gateway's orders go out under; okx keeps a leverage for each
    std::expected<MarginState, exec::VenueError> margin_state(InstrumentId instrument) {
        const auto symbol = margin_symbol(instrument);
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        const auto mode = modes_->mode(instrument);
        const auto body = pull(builder_.leverage_info(*symbol, mgn_mode_of(mode), state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto lever = parse_leverage_info(*body);
        if (!lever.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(lever.error()))};
        }
        MarginState state{};
        state.leverage = *lever;
        state.mode = mode;
        return state;
    }

    std::expected<void, exec::VenueError> set_leverage(InstrumentId instrument, std::uint32_t leverage) {
        const auto symbol = margin_symbol(instrument);
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        const auto mode = modes_->mode(instrument);
        bool both_sides = false;
        if (mode == MarginMode::Isolated) {
            // isolated in long/short mode keeps a leverage a side, and posSide is required
            const auto config = pull(builder_.account_config(state_->venue_now()));
            if (!config.has_value()) {
                return std::unexpected{config.error()};
            }
            const auto pos_mode = parse_position_mode(*config);
            if (!pos_mode.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(pos_mode.error()))};
            }
            both_sides = *pos_mode == "long_short_mode";
        }
        const auto send = [&](std::string_view side) -> std::expected<void, exec::VenueError> {
            const auto body = pull(
                builder_.set_leverage(*symbol, leverage, mgn_mode_of(mode), state_->venue_now(), side));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            return {};
        };
        if (!both_sides) {
            return send({});
        }
        if (const auto long_side = send("long"); !long_side.has_value()) {
            return long_side;
        }
        return send("short");
    }

    // no venue call: the next order on the instrument goes out under the chosen tdMode
    std::expected<void, exec::VenueError> set_margin_mode(InstrumentId instrument, MarginMode mode) {
        const auto symbol = margin_symbol(instrument);
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        const auto config = pull(builder_.account_config(state_->venue_now()));
        if (!config.has_value()) {
            return std::unexpected{config.error()};
        }
        if (const auto level = parse_account_mode(*config); level.has_value() && *level == AccountMode::Spot) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "the okx account is in spot mode (acctLv 1)")};
        }
        // an open position would be closed under the other mode, and a reduce-only order there is refused
        const auto positions = position_reports(instrument.market());
        if (!positions.has_value()) {
            return std::unexpected{positions.error()};
        }
        for (const auto& report : *positions) {
            if (report.instrument == instrument && !report.net_qty.is_undef() && report.net_qty.raw != 0) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal,
                    0,
                    "the instrument has an open position; close it before its orders change margin mode")};
            }
        }
        if (!modes_->set(instrument, mode)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "this gateway already holds the most isolated instruments it can")};
        }
        return {};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) {
        switch (market) {
        case MarketType::Perp:
        case MarketType::Future:
        case MarketType::Option:
            break;
        case MarketType::Spot:
            // positions refuses instType SPOT; spot inventory is a per-currency balance
            return std::unexpected{exec::not_implemented("okx spot positions: the venue reports balances, not "
                                                         "positions")};
        }
        const auto body = pull(builder_.positions(market, state_->venue_now()));
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
            if (const auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::string, exec::VenueError> option_account_mode() {
        const auto config = pull(builder_.account_config(state_->venue_now()));
        if (!config.has_value()) {
            return std::unexpected{config.error()};
        }
        const auto mode = parse_account_mode(*config);
        if (!mode.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(mode.error()))};
        }
        return std::string{option_mode_name(*mode)};
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        // one trading account backs both markets
        (void)market;
        // asked first; a failed mode read leaves available undef
        auto mode = AccountMode::Unknown;
        if (const auto config = pull(builder_.account_config(state_->venue_now())); config.has_value()) {
            if (const auto read = parse_account_mode(*config); read.has_value()) {
                mode = *read;
            }
        }
        const auto body = pull(builder_.balances(state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BalanceBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_balances(*body, storage);
        if (!rows.has_value()) {
            // an unreadable wallet is refused rather than reported empty
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<BalanceReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (const auto report = map_balance(wire, mode, state_->clock->wall()); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    [[nodiscard]] static std::string_view mgn_mode_of(MarginMode mode) noexcept {
        return mode == MarginMode::Isolated ? "isolated" : "cross";
    }

    [[nodiscard]] std::expected<std::string_view, exec::VenueError> margin_symbol(InstrumentId instrument) const {
        const auto market = instrument.market();
        if (market != MarketType::Perp && market != MarketType::Future) {
            return std::unexpected{exec::margin_unsupported("okx leverage is set here on swaps and futures")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "instrument is not in this gateway's catalogue")};
        }
        return symbol;
    }

    TradeModes* modes_{&TradeModes::process()};
    VenueState* state_;
    net::RestClient rest_;
    RequestBuilder builder_;
};

} // namespace raikiri::venues::okx::internal
