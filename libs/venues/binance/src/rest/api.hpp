#pragma once

// signed calls and the shared weight pool; unsigned market-data polls live beside the socket

#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/orders.hpp>
#include <raikiri/venues/binance/positions.hpp>
#include <raikiri/venues/binance/rest.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::binance::internal {

// missing credentials are AuthFailure so the gateway halts instead of retrying
exec::VenueError build_failed(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NoCredentials ? exec::ErrorClass::AuthFailure
                                                                   : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

class RestApi {
public:
    explicit RestApi(VenueState& state)
        : state_{&state},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}} {
        if (state.market == MarketType::Option) {
            option_rest_.emplace(
                state.io,
                net::RestConfig{.host = std::string{rest_host(state.testnet, MarketType::Option)}, .port = "443"});
        }
    }

    void start() { signed_rest().start(); }

    void stop() { signed_rest().stop(); }

    // keeps the socket warm so an order does not pay a reconnect
    void keepalive(UnixNanos now) noexcept { signed_rest().keepalive(now); }

    [[nodiscard]] std::string_view order_path() const noexcept {
        return option_rest_ ? ep::k_option_order : ep::k_order;
    }

    // held by the state so every signer shares one builder and one clock offset
    [[nodiscard]] RequestBuilder& builder() noexcept { return state_->builder; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return signed_rest().send(request, std::move(on_done));
    }

    // binance meters weight across every endpoint, so every call spends Shared
    [[nodiscard]] std::expected<void, exec::VenueError> spend(std::string_view target, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited
        }
        const auto taken = state_->budget->take(target, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::RateLimited,
                                   static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
                                   "local rate budget spent; retry after the stated milliseconds")};
    }

    void penalize() const noexcept {
        if (state_->budget != nullptr) {
            // the venue's refusal outranks our accounting
            state_->budget->penalize_all(state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    // cold path only: reconciliation reads and listen-key calls
    [[nodiscard]] std::expected<std::string, exec::VenueError> pull(const net::RestRequest& request,
                                                                    std::uint64_t timeout_ms = 10'000) {
        return usable_body(await_response(*state_, signed_rest(), request, timeout_ms));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) {
        const auto request = builder().open_orders(state_->venue_now());
        const auto path = option_rest_ ? ep::k_option_open_orders : ep::k_open_orders;
        if (const auto allowed = spend(path, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OrderBuffer>();
        auto& storage = *storage_owner;
        const auto wire = option_rest_ ? parse_option_orders(*body, storage) : parse_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::OrderStatusReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_open_order(entry, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id, MarketType) {
        if (option_rest_) {
            return option_query_order(id);
        }
        if (const auto allowed = spend(ep::k_order, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto* instrument = first_known_instrument();
        if (instrument == nullptr) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "query needs a loaded catalogue")};
        }
        const auto request = builder().query_order(
            base_of(*instrument).exchange_symbol.view(), id, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto report = map_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unmappable order")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType, UnixNanos since) {
        if (option_rest_) {
            return option_fill_reports(since);
        }
        if (const auto allowed = spend(ep::k_user_trades, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        // the whole account; the endpoint takes no symbol
        const auto request = builder().user_trades({}, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<TradeBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_user_trades(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_user_trade(entry, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) {
        if (market == MarketType::Option) {
            return option_balance_reports();
        }
        if (const auto allowed = spend(ep::k_account, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        if (market != MarketType::Perp) {
            // fapi is usd-m futures only; spot balances need another api
            return std::unexpected{exec::not_implemented("binance spot balance pull")};
        }
        const auto request = builder().account(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<BalanceBuffer>();
        auto& storage = *storage_owner;
        const auto wire = parse_account(*body, storage);
        if (!wire.has_value()) {
            // an unreadable account is refused, since an empty vector reads as holding nothing
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<BalanceReport> out;
        out.reserve(wire->assets.size());
        for (const auto& entry : wire->assets) {
            // binance names every collateral asset it accepts, held or not
            if (const auto report = map_balance(entry, wire->multi_assets_margin); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> option_balance_reports() {
        if (!option_rest_) {
            return std::unexpected{exec::not_implemented("binance option balances on a gateway of another market")};
        }
        const auto path = state_->option_margin_account ? ep::k_option_margin_account : ep::k_option_account;
        if (const auto allowed = spend(path, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto request = builder().option_account(state_->venue_now(), state_->option_margin_account);
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, *option_rest_, *request, 10'000));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<OptionAccountBuffer>();
        const auto wire = parse_option_account(*body, *storage_owner);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<BalanceReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_option_balance(entry, state_->clock->wall()); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType, UnixNanos since) {
        if (option_rest_) {
            return std::unexpected{exec::not_implemented("binance options pay no funding")};
        }
        if (const auto allowed = spend(ep::k_income, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto request = builder().funding_income(since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage_owner = std::make_unique<IncomeBuffer>();
        auto& storage = *storage_owner;
        const auto rows = parse_income(*body, storage);
        if (!rows.has_value()) {
            // an unreadable body is an error; an empty ledger would book zero funding
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable income ledger")};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& row : *rows) {
            LedgerKind kind{};
            if (row.income_type == "FUNDING_FEE") {
                kind = LedgerKind::Funding;
            } else if (row.income_type == "COMMISSION") {
                kind = LedgerKind::Fee;
            } else {
                // realized pnl and transfers derive from fills already booked
                continue;
            }
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto amount = parse_decimal_e9(row.income);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero one
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            charge.amount = *amount;
            charge.kind = kind;
            charge.settled_at = UnixNanos{row.time_ms * 1'000'000};
            char buf[32];
            const auto id_for_reference = kind == LedgerKind::Fee && row.trade_id != 0 ? row.trade_id : row.tran_id;
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, id_for_reference);
            if (ec == std::errc{}) {
                if (auto id = ExecId::from(std::string_view{buf, static_cast<std::size_t>(end - buf)});
                    id.has_value()) {
                    charge.reference = *id;
                }
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType) {
        const auto path = option_rest_ ? ep::k_option_position : ep::k_position_risk;
        if (const auto allowed = spend(path, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto request = builder().position_risk(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::array<WirePositionRisk, 512> storage{};
        const auto wire = option_rest_ ? parse_option_positions(*body, storage) : parse_position_risk(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::PositionReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_position(entry, state_->instruments); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

private:
    // an option gateway does everything signed on eapi and never opens the futures host
    [[nodiscard]] net::RestClient& signed_rest() noexcept { return option_rest_ ? *option_rest_ : rest_; }

    // the single-order read needs a symbol a client id does not carry, so the open orders answer it
    std::expected<exec::OrderStatusReport, exec::VenueError> option_query_order(const ExchangeClientOrderId& id) {
        const auto reports = open_order_reports(MarketType::Option);
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

    // a whole-account trade pull does not exist here: the bill and the held positions say which symbols to ask
    std::expected<std::vector<exec::FillReport>, exec::VenueError> option_fill_reports(UnixNanos since) {
        if (const auto allowed = spend(ep::k_option_bill, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto bill_request = builder().option_bill(since, state_->venue_now());
        if (!bill_request.has_value()) {
            return std::unexpected{build_failed(bill_request.error())};
        }
        const auto bill_body = pull(*bill_request);
        if (!bill_body.has_value()) {
            return std::unexpected{bill_body.error()};
        }
        static thread_local const auto bill_owner = std::make_unique<OptionBillBuffer>();
        const auto bill = parse_option_bill(*bill_body, *bill_owner);
        if (!bill.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(bill.error()))};
        }

        if (const auto allowed = spend(ep::k_option_position, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto position_request = builder().position_risk(state_->venue_now());
        if (!position_request.has_value()) {
            return std::unexpected{build_failed(position_request.error())};
        }
        const auto position_body = pull(*position_request);
        if (!position_body.has_value()) {
            return std::unexpected{position_body.error()};
        }
        std::array<WirePositionRisk, 512> position_storage{};
        const auto positions = parse_option_positions(*position_body, position_storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }

        static thread_local const auto trades_owner = std::make_unique<TradeBuffer>();
        std::vector<exec::FillReport> out;
        for (const auto symbol : option_traded_symbols(*bill, *positions)) {
            if (const auto allowed = spend(ep::k_option_user_trades, exec::RateClass::Poll); !allowed.has_value()) {
                return std::unexpected{allowed.error()};
            }
            const auto request = builder().user_trades(symbol, since, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = pull(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            const auto wire = parse_option_user_trades(*body, *trades_owner);
            if (!wire.has_value()) {
                return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& entry : *wire) {
                if (const auto report = map_user_trade(entry, state_->instruments); report.has_value()) {
                    out.push_back(*report);
                }
            }
        }
        return out;
    }

    [[nodiscard]] const Instrument* first_known_instrument() const noexcept {
        const auto all = state_->instruments.all();
        return all.empty() ? nullptr : &all.front();
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    usable_body(const std::optional<net::RestResponse>& response) const {
        if (!response.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body); err.has_value() && err->code != k_ok) {
            const auto cls = classify(err->code);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{exec::VenueError::make(cls, err->code, err->msg)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
    // eapi lives on its own host, for keys, orders and pulls alike; only an option gateway has it
    std::optional<net::RestClient> option_rest_;
};

} // namespace raikiri::venues::binance::internal
