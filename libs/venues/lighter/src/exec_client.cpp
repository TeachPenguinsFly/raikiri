#include <raikiri/book/ladder.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/lighter/account.hpp>
#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/venues/lighter/lifecycle.hpp>
#include <raikiri/venues/lighter/tx.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::lighter::internal {

namespace {

constexpr std::uint32_t k_trade_stream_id = 1;
constexpr std::uint32_t k_account_stream_id = 2;

constexpr std::uint64_t k_default_ws_timeout_ns = 10'000'000'000ULL;
constexpr std::uint64_t k_rest_timeout_ms = 10'000;
constexpr std::size_t k_max_unanswered = 512;
constexpr std::size_t k_trade_in_flight = 32;

// the reference signer's default, a second short of ten minutes
constexpr std::int64_t k_tx_lifetime_ms = 10 * 60 * 1000 - 1000;
// a resting order gets the reference sdk's 28 days; the venue allows 5 minutes to 30 days
constexpr std::int64_t k_resting_lifetime_ms = 28LL * 24 * 60 * 60 * 1000;
// the venue caps a token at 8 h
constexpr std::int64_t k_token_lifetime_s = std::int64_t{7} * 60 * 60;
constexpr std::int64_t k_token_renew_s = std::int64_t{6} * 60 * 60;

constexpr std::int64_t k_max_base_amount = (std::int64_t{1} << 48) - 1;
constexpr std::int64_t k_max_price = 0xFFFFFFFFLL;
// skip-nonce accepts any nonce above the last and below 2^47 - 1
constexpr std::int64_t k_max_skip_nonce = (std::int64_t{1} << 47) - 2;

[[nodiscard]] std::int64_t wall_ms(const Clock& clock) noexcept {
    return static_cast<std::int64_t>(clock.wall().ns / 1'000'000ULL);
}

void append_form_value(std::string& out, std::string_view value) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += c;
            continue;
        }
        out += '%';
        out += digits[byte >> 4];
        out += digits[byte & 0x0FU];
    }
}

[[nodiscard]] std::optional<std::int64_t> steps(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// the frame the venue's socket takes; tx_info is embedded as an object, not a string
class TxCodec final : public net::WsRpcCodec {
public:
    [[nodiscard]] std::optional<std::string> login(UnixNanos) override { return std::string{}; }

    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override {
        const auto reply = parse_send_reply(payload);
        if (!reply.has_value() || reply->id.empty()) {
            return {};
        }
        std::uint64_t id = 0;
        const auto [stop, ec] = std::from_chars(reply->id.data(), reply->id.data() + reply->id.size(), id);
        if (ec != std::errc{} || stop != reply->id.data() + reply->id.size()) {
            return {};
        }
        return {.kind = net::WsRpcFrame::Kind::Reply, .id = id, .ok = reply->ok(), .detail = reply->message};
    }
};

class LighterExecution final : public exec::ExecutionClient, private net::WsHandler {
public:
    explicit LighterExecution(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, rest_{state_->io,
                                          net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                                          .port = "443",
                                                          .pool_size = 2,
                                                          .timeout_ms = k_rest_timeout_ms}},
          account_ws_{state_->io,
                      net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                    .port = "443",
                                    .target = std::string{stream_path()}},
                      *this},
          orders_{*state_->sink, state_->instruments, state_->account_index} {}

    std::expected<void, exec::VenueError> start() override {
        if (!state_->account_parsed || !state_->key.loaded()) {
            // a market-data gateway carries no account; orders are then refused one by one
            return {};
        }
        rest_.start();
        if (const auto synced = resync_blocking(); !synced.has_value()) {
            return std::unexpected{synced.error()};
        }
        open_trade_socket();
        account_ws_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        if (trade_) {
            trade_->stop();
            trade_.reset();
        }
        account_ws_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return started_ && account_ws_.connected(); }

    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        if (trade_) {
            trade_->poll();
        }
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
        }
    }

    void submit(const OrderCmd& cmd) noexcept override {
        auto tx = create_tx(cmd);
        if (!tx.has_value()) {
            refuse(cmd, Kind::Create, 0, tx.error());
            return;
        }
        const auto digest = tx_hash(*tx, chain_id(state_->testnet));
        orders_.track(cmd);
        send(cmd, Kind::Create, k_tx_create_order, tx_info(*tx, state_->key.sign(digest)));
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto market = market_of(cmd.instrument);
        if (!ready_to_sign() || !market.has_value()) {
            refuse(cmd, Kind::Cancel, 0, !market.has_value() ? "instrument not on lighter" : "no api key loaded");
            return;
        }
        std::int64_t index = client_order_index(cmd.exchange_client_order_id);
        if (cmd.exchange_client_order_id.empty()) {
            const auto target = cmd.target_exchange_order_id.view();
            const auto [stop, ec] = std::from_chars(target.data(), target.data() + target.size(), index);
            if (ec != std::errc{} || target.empty()) {
                refuse(cmd, Kind::Cancel, 0, "cancel names neither a client nor a venue order id");
                return;
            }
        }
        const CancelOrderTx tx{.header = header(), .market_index = *market, .index = index};
        send(cmd,
             Kind::Cancel,
             k_tx_cancel_order,
             tx_info(tx, state_->key.sign(tx_hash(tx, chain_id(state_->testnet)))));
    }

    void amend(const OrderCmd& cmd) noexcept override {
        refuse(cmd, Kind::Amend, 0, "lighter amend is not wired; cancel and replace");
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) override {
        const auto body = get(std::format("{}?account_index={}", ep::k_active_orders, account()), true);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<WireOrder> rows;
        if (const auto parsed = parse_account_orders(*body, rows); !parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        std::vector<exec::OrderStatusReport> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            if (const auto report = status_report(row); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType) override {
        const auto body = get(
            std::format(
                "{}?account_index={}&client_order_indexes={}", ep::k_account_orders, account(), client_order_index(id)),
            true);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<WireOrder> rows;
        if (const auto parsed = parse_account_orders(*body, rows); !parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        for (const auto& row : rows) {
            if (auto report = status_report(row); report.has_value()) {
                report->exchange_client_order_id = id;
                return *report;
            }
        }
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "lighter: no such order")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType, UnixNanos since) override {
        const auto body = get(
            std::format("{}?sort_by=timestamp&sort_dir=desc&limit=100&account_index={}", ep::k_trades, account()),
            true);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<WireTrade> rows;
        if (const auto parsed = parse_account_trades(*body, rows); !parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        std::vector<exec::FillReport> out;
        for (const auto& row : rows) {
            if (row.ts_ms * 1'000'000ULL < since.ns) {
                continue;
            }
            const auto* instrument = state_->instrument_of(row.market_id);
            const auto ours = side_of(row, state_->account_index);
            const auto price = instrument == nullptr ? std::nullopt : steps(row.price, base_of(*instrument).tick_size);
            const auto qty = instrument == nullptr ? std::nullopt : steps(row.size, base_of(*instrument).lot_size);
            if (!price.has_value() || !qty.has_value()) {
                continue;
            }
            exec::FillReport fill{};
            fill.exec_id = trade_id_of(row.trade_id);
            fill.exchange_order_id = order_id_of(ours.order_index);
            fill.instrument = base_of(*instrument).id;
            fill.side = ours.is_ask ? Side::Sell : Side::Buy;
            fill.price = PriceTicks::from_raw(*price);
            fill.qty = QtyLots::from_raw(*qty);
            fill.fee = fee_of(row, ours.maker);
            fill.liquidity = ours.maker ? Liquidity::Maker : Liquidity::Taker;
            fill.ts_venue = UnixNanos{row.ts_ms * 1'000'000ULL};
            out.push_back(fill);
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        WireAccount row{};
        const auto body = account_row(row);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<exec::PositionReport> out;
        for (const auto& position : row.positions) {
            const auto* instrument = state_->instrument_of(position.market_id);
            if (instrument == nullptr || ::raikiri::market_of(*instrument) != market) {
                continue;
            }
            const auto& base = base_of(*instrument);
            const auto qty = steps(position.position, base.lot_size);
            if (!qty.has_value()) {
                continue;
            }
            exec::PositionReport report{};
            report.instrument = base.id;
            report.net_qty = QtyLots::from_raw(position.sign < 0 ? -*qty : *qty);
            if (const auto entry = steps(position.avg_entry_price, base.tick_size); entry.has_value()) {
                report.avg_entry = PriceTicks::from_raw(*entry);
            }
            report.ts_venue = state_->clock->wall();
            out.push_back(report);
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType) override {
        WireAccount row{};
        const auto body = account_row(row);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        BalanceReport usdc{};
        if (const auto code = to_asset_code("USDC"); code.has_value()) {
            usdc.asset = *code;
        }
        if (const auto total = parse_amount_e9(row.collateral); total.has_value()) {
            usdc.total = *total;
        }
        if (const auto available = parse_amount_e9(row.available_balance); available.has_value()) {
            usdc.available = *available;
        }
        usdc.ts_venue = state_->clock->wall();
        return std::vector<BalanceReport>{usdc};
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] bool ready_to_sign() const noexcept { return state_->account_parsed && state_->key.loaded(); }

    [[nodiscard]] std::string account() const { return std::to_string(state_->account_index); }

    [[nodiscard]] std::optional<std::int16_t> market_of(InstrumentId instrument) const noexcept {
        const auto found = state_->market_of.find(instrument.packed);
        if (found == state_->market_of.end() || found->second > 0x7FFF) {
            return std::nullopt;
        }
        return static_cast<std::int16_t>(found->second);
    }

    [[nodiscard]] TxHeader header() noexcept {
        const auto now = wall_ms(*state_->clock);
        auto nonce = last_nonce_ + 1;
        if (skip_nonce_) {
            nonce = std::max(nonce, now);
        }
        last_nonce_ = nonce;
        return {.account_index = state_->account_index,
                .api_key_index = state_->api_key_index,
                .expired_at = now + k_tx_lifetime_ms,
                .nonce = nonce,
                .skip_nonce = skip_nonce_ && nonce <= k_max_skip_nonce};
    }

    [[nodiscard]] std::expected<CreateOrderTx, std::string_view> create_tx(const OrderCmd& cmd) noexcept {
        if (!ready_to_sign()) {
            return std::unexpected{"no api key loaded"};
        }
        const auto market = market_of(cmd.instrument);
        if (!market.has_value()) {
            return std::unexpected{"instrument not on lighter"};
        }
        if (cmd.tif == TimeInForce::Fok) {
            return std::unexpected{"lighter has no fill-or-kill"};
        }
        if (cmd.price.raw <= 0 || cmd.price.raw > k_max_price) {
            return std::unexpected{"price outside the venue's 32-bit range; a market order needs a worst price too"};
        }
        if (cmd.qty.raw <= 0 || cmd.qty.raw > k_max_base_amount) {
            return std::unexpected{"quantity outside the venue's 48-bit range"};
        }
        if (cmd.exchange_client_order_id.empty()) {
            return std::unexpected{"order has no client order id"};
        }
        const bool post_only = has_flag(cmd.flags, OrderFlags::PostOnly);
        const bool immediate = cmd.order_type == OrderType::Market || cmd.tif == TimeInForce::Ioc;
        if (post_only && immediate) {
            return std::unexpected{"post-only cannot be immediate"};
        }
        CreateOrderTx tx{.header = {},
                         .market_index = *market,
                         .client_order_index = client_order_index(cmd.exchange_client_order_id),
                         .base_amount = cmd.qty.raw,
                         .price = static_cast<std::uint32_t>(cmd.price.raw),
                         .is_ask = static_cast<std::uint8_t>(cmd.side == Side::Sell ? 1 : 0),
                         .type = cmd.order_type == OrderType::Market ? k_order_market : k_order_limit,
                         .time_in_force = immediate   ? k_tif_ioc
                                          : post_only ? k_tif_post_only
                                                      : k_tif_gtt,
                         .reduce_only = static_cast<std::uint8_t>(has_flag(cmd.flags, OrderFlags::ReduceOnly) ? 1 : 0)};
        tx.header = header();
        tx.order_expiry = immediate ? 0 : wall_ms(*state_->clock) + k_resting_lifetime_ms;
        return tx;
    }

    void send(const OrderCmd& cmd, Kind kind, std::uint8_t type, const std::string& info) noexcept {
        if (trade_ && trade_->ready()) {
            const auto id = trade_->next_id();
            std::string frame = R"({"type":"jsonapi/sendtx","data":{"id":")" + std::to_string(id) + R"(","tx_type":)" +
                                std::to_string(type) + R"(,"tx_info":)" + info + "}}";
            if (trade_->request(id, frame, [this, cmd, kind](const net::WsRpcReply& reply) {
                    if (reply.outcome == net::WsRpcOutcome::Unknown) {
                        exec::report_unknown_outcome(*state_->sink,
                                                     exec::in_flight_of(cmd, state_->clock->mono()),
                                                     state_->clock->wall(),
                                                     reply.detail);
                        return;
                    }
                    on_reply(reply.payload, cmd, kind);
                })) {
                return;
            }
        }
        if (state_->rest_budget != nullptr &&
            !state_->rest_budget->take(ep::k_send_tx, state_->clock->mono(), exec::RateClass::Order).has_value()) {
            refuse(cmd, kind, 23000, "local sendTx budget spent");
            return;
        }
        form_ = "tx_type=" + std::to_string(type) + "&tx_info=";
        append_form_value(form_, info);
        const net::RestRequest request{.method = net::HttpMethod::Post,
                                       .target = ep::k_send_tx,
                                       .body = form_,
                                       .headers = {},
                                       .content_type = "application/x-www-form-urlencoded"};
        if (!rest_.send(request, [this, cmd, kind](net::RestResponse&& response) {
                (void)unanswered_.resolve(cmd.client_order_id);
                if (response.transport_failed()) {
                    // it may have reached the sequencer, so this is not a rejection
                    state_->sink->on_order_error(
                        cmd.client_order_id, exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response.error));
                    return;
                }
                on_reply(response.body, cmd, kind);
            })) {
            refuse(cmd, kind, 0, "request not queued");
            return;
        }
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    // a 200 is the api server's acceptance; the account stream reports what the sequencer made of it
    void on_reply(std::string_view payload, const OrderCmd& cmd, Kind kind) noexcept {
        const auto reply = parse_send_reply(payload);
        if (!reply.has_value()) {
            refuse(cmd, kind, 0, "unreadable sendTx answer");
            return;
        }
        if (reply->ok()) {
            return;
        }
        if (reply->code == k_code_bad_attributes && skip_nonce_) {
            skip_nonce_ = false;
        }
        if (reply->code == k_code_invalid_nonce || reply->code == k_code_bad_attributes) {
            resync_async();
        }
        refuse(cmd, kind, reply->code, reply->message);
    }

    // a rejected tx leaves the nonce unused, which a sequential key must fill
    void resync_async() noexcept {
        if (resyncing_) {
            return;
        }
        resyncing_ = true;
        nonce_target_ = std::format(
            "{}?account_index={}&api_key_index={}", ep::k_next_nonce, account(), state_->api_key_index);
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = nonce_target_};
        if (!rest_.send(request, [this](net::RestResponse&& response) {
                resyncing_ = false;
                if (!response.transport_failed() && response.status == 200) {
                    adopt_nonce(response.body);
                }
            })) {
            resyncing_ = false;
        }
    }

    void adopt_nonce(std::string_view body) noexcept {
        const auto next = parse_next_nonce(body);
        if (!next.has_value()) {
            return;
        }
        last_nonce_ = skip_nonce_ ? std::max(last_nonce_, *next - 1) : *next - 1;
    }

    [[nodiscard]] std::expected<void, exec::VenueError> resync_blocking() {
        const auto body = get(
            std::format("{}?account_index={}&api_key_index={}", ep::k_next_nonce, account(), state_->api_key_index),
            false);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        if (!parse_next_nonce(*body).has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "lighter: no nonce for this account and key")};
        }
        adopt_nonce(*body);
        return {};
    }

    [[nodiscard]] std::string token() {
        const auto now_s = wall_ms(*state_->clock) / 1000;
        if (token_.empty() || now_s >= token_made_s_ + k_token_renew_s) {
            token_ = auth_token(state_->key, now_s + k_token_lifetime_s, state_->account_index, state_->api_key_index);
            token_made_s_ = now_s;
        }
        return token_;
    }

    // cold: reconciliation pulls only
    [[nodiscard]] std::expected<std::string, exec::VenueError> get(const std::string& target, bool authorized) {
        if (!ready_to_sign()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "lighter: no account")};
        }
        if (state_->rest_budget != nullptr &&
            !state_->rest_budget->take(target, state_->clock->mono(), exec::RateClass::Poll).has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, 0, "lighter: local rest budget spent")};
        }
        const auto auth = authorized ? token() : std::string{};
        net::RestRequest request{.method = net::HttpMethod::Get, .target = target};
        if (authorized) {
            request.headers.push_back({.name = "authorization", .value = auth});
        }
        rest_.start();
        const auto response = net::await_response(state_->io, *state_->clock, rest_, request, 2 * k_rest_timeout_ms);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "lighter: no response")};
        }
        if (response->status == 429 || response->status == 405) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::RateLimited, response->status, "lighter: rate limited")};
        }
        if (response->status != 200) {
            const auto reply = parse_send_reply(response->body);
            const auto code = reply.has_value() ? reply->code : response->status;
            return std::unexpected{exec::VenueError::make(classify_code(code), code, response->body)};
        }
        return response->body;
    }

    [[nodiscard]] std::expected<void, exec::VenueError> account_row(WireAccount& row) {
        const auto body = get(std::format("{}?by=index&value={}", ep::k_account, account()), false);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        account_body_ = *body;
        if (const auto parsed = parse_account(account_body_, row); !parsed.has_value()) {
            return std::unexpected{unreadable(parsed.error())};
        }
        return {};
    }

    [[nodiscard]] static exec::VenueError unreadable(WireError err) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, wire::to_string(err));
    }

    [[nodiscard]] std::optional<exec::OrderStatusReport> status_report(const WireOrder& row) const {
        const auto* instrument = state_->instrument_of(row.market_index);
        if (instrument == nullptr) {
            return std::nullopt;
        }
        const auto& base = base_of(*instrument);
        const auto price = steps(row.price, base.tick_size);
        const auto qty = steps(row.initial_base_amount, base.lot_size);
        const auto leaves = steps(row.remaining_base_amount, base.lot_size);
        if (!price.has_value() || !qty.has_value() || !leaves.has_value()) {
            return std::nullopt;
        }
        exec::OrderStatusReport report{};
        if (const auto* cmd = orders_.find(row.client_order_index); cmd != nullptr) {
            report.exchange_client_order_id = cmd->exchange_client_order_id;
        }
        report.exchange_order_id = order_id_of(row.order_index);
        report.instrument = base.id;
        report.side = row.is_ask ? Side::Sell : Side::Buy;
        const auto phase = order_phase(row.status);
        report.status = phase == OrderPhase::Live
                            ? (*leaves < *qty ? OrderStatus::PartiallyFilled : OrderStatus::Accepted)
                        : phase == OrderPhase::Filled ? OrderStatus::Filled
                                                      : OrderStatus::Canceled;
        report.price = PriceTicks::from_raw(*price);
        report.qty = QtyLots::from_raw(*qty);
        report.leaves = QtyLots::from_raw(phase == OrderPhase::Live ? *leaves : 0);
        report.ts_venue = UnixNanos{row.updated_ms * 1'000'000ULL};
        return report;
    }

    void open_trade_socket() {
        net::WsRpcConfig config{};
        config.ws.host = std::string{stream_host(state_->testnet)};
        config.ws.port = "443";
        config.ws.target = std::string{stream_path()};
        config.request_timeout_ns = state_->order_answer_timeout_ns != 0 ? state_->order_answer_timeout_ns
                                                                         : k_default_ws_timeout_ns;
        config.max_in_flight = k_trade_in_flight;
        trade_ = std::make_unique<net::WsRpc>(state_->io, *state_->clock, std::move(config), codec_);
        trade_->set_admit([this] {
            return state_->rest_budget == nullptr ||
                   state_->rest_budget->take(ep::k_ws_send_tx, state_->clock->mono(), exec::RateClass::Order)
                       .has_value();
        });
        trade_->set_observer([this](net::WsRpcState rpc, std::string_view) {
            if (rpc == net::WsRpcState::LoggingIn) {
                return;
            }
            state_->sink->on_connectivity(
                {.state = rpc == net::WsRpcState::Ready ? exec::ConnState::Connected : exec::ConnState::Disconnected,
                 .stream_id = k_trade_stream_id,
                 .ts = state_->clock->wall()});
        });
        trade_->start();
    }

    void on_open() override {
        const auto auth = token();
        const auto id = account();
        for (const std::string_view channel : {"account_all_orders/", "account_all_trades/"}) {
            const auto frame = std::format(R"({{"type":"subscribe","channel":"{}{}","auth":"{}"}})", channel, id, auth);
            (void)account_ws_.send(frame);
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = k_account_stream_id, .ts = state_->clock->wall()});
    }

    void on_close(net::WsCloseReason) override {
        // a fresh token on the next open
        token_.clear();
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = k_account_stream_id, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        if (payload.starts_with(R"({"error")")) {
            on_stream_error(payload);
            return;
        }
        if (payload.find("\"account_all_trades") != std::string_view::npos) {
            if (parse_account_trades(payload, trade_rows_).has_value()) {
                for (const auto& trade : trade_rows_) {
                    orders_.on_trade(trade, received);
                }
            }
            return;
        }
        if (payload.find("\"account_all_orders") != std::string_view::npos) {
            if (parse_account_orders(payload, order_rows_).has_value()) {
                for (const auto& order : order_rows_) {
                    orders_.on_order(order, received);
                }
            }
        }
    }

    void on_stream_error(std::string_view payload) noexcept {
        if (const auto error = parse_stream_error(payload); error.has_value() && error->code != 0) {
            state_->last_error_code = error->code;
            if (classify_code(error->code) == exec::ErrorClass::AuthFailure) {
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = k_account_stream_id,
                                               .ts = state_->clock->wall()});
            }
        }
    }

    void refuse(const OrderCmd& cmd, Kind kind, std::int64_t code, std::string_view reason) noexcept {
        if (kind == Kind::Create) {
            orders_.forget(cmd.exchange_client_order_id);
        }
        const auto cls = code == 0 ? exec::ErrorClass::Terminal : classify_code(code);
        state_->sink->on_order_error(cmd.client_order_id, exec::VenueError::make(cls, code, reason));
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.reject_class = cls;
        event.reject_code = static_cast<std::int32_t>(code);
        event.price = cmd.price;
        event.qty = cmd.qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
        if (cls == exec::ErrorClass::AuthFailure) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_trade_stream_id, .ts = state_->clock->wall()});
        }
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::WsClient account_ws_;
    TxCodec codec_;
    std::unique_ptr<net::WsRpc> trade_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};

    std::int64_t last_nonce_{-1};
    bool skip_nonce_{true};
    bool resyncing_{false};
    std::string nonce_target_;
    std::string form_;
    std::string token_;
    std::int64_t token_made_s_{0};
    std::string account_body_;

    Lifecycle orders_;
    std::vector<WireOrder> order_rows_;
    std::vector<WireTrade> trade_rows_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<LighterExecution>(std::move(state));
}

} // namespace raikiri::venues::lighter::internal
