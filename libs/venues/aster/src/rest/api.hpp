#pragma once

// signed calls and the shared weight pool; unsigned market-data polls live beside the socket

#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/orders.hpp>
#include <raikiri/venues/aster/positions.hpp>
#include <raikiri/venues/aster/rest.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../state.hpp"

namespace raikiri::venues::aster::internal {

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
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(state.testnet)}, .port = "443"}} {}

    void start() { rest_.start(); }

    void stop() { rest_.stop(); }

    // keeps the socket warm so an order does not pay a reconnect
    void keepalive(UnixNanos now) noexcept { rest_.keepalive(now); }

    // held by the state so every signer shares one builder and one clock offset
    [[nodiscard]] RequestBuilder& builder() noexcept { return state_->builder; }

    [[nodiscard]] bool send(const net::RestRequest& request, net::RestCallback on_done) {
        return rest_.send(request, std::move(on_done));
    }

    // aster meters weight across every endpoint
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
        return usable_body(await_response(*state_, rest_, request, timeout_ms));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) {
        const auto request = builder().open_orders(state_->venue_now());
        if (const auto allowed = spend(ep::k_open_orders, exec::RateClass::Poll); !allowed.has_value()) {
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
        const auto wire = parse_orders(*body, storage);
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
        if (const auto allowed = spend(ep::k_account, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        if (market != MarketType::Perp) {
            // fapi is usd-m futures only; spot balances need another api
            return std::unexpected{exec::not_implemented("aster spot balance pull")};
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
            // aster names every collateral asset it accepts, held or not
            if (const auto report = map_balance(entry, wire->multi_assets_margin); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType, UnixNanos since) {
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
        if (const auto allowed = spend(ep::k_position_risk, exec::RateClass::Poll); !allowed.has_value()) {
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
        const auto wire = parse_position_risk(*body, storage);
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
            const auto cls = classify(err->code, err->msg);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{exec::VenueError::make(cls, err->code, err->msg)};
        }
        return response->body;
    }

    VenueState* state_;
    net::RestClient rest_;
};

} // namespace raikiri::venues::aster::internal
