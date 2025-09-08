#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::gate::detail {

namespace {

constexpr std::uint32_t k_private_stream_id = 1;

// Routes one private frame. The market decides which of Gate's two order
// shapes the payload is; nothing in the frame itself says.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        static thread_local OrderBuffer orders;
        const auto wire = parse_ws_orders(frame, orders, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            // Remembered BEFORE the event is emitted: a user trade for the
            // same fill may already be queued behind this frame, and it can
            // only state its leaves out of this memo.
            state.remember(entry);
            if (const auto mapped = map_order_event(entry, state.instruments, received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::UserTrade: {
        static thread_local UserTradeBuffer trades;
        const auto wire = parse_ws_user_trades(frame, trades, market);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            const auto memo = state.memo_for(entry.order_id);
            if (const auto mapped = map_user_trade(
                    entry, state.instruments, memo.leaves, memo.client_order_id, received);
                mapped.has_value()) {
                state.sink->on_order_event(*mapped);
            }
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            // Code 4 wraps a REST-style label in its message; the number is
            // enough to tell a bad key from an unknown contract.
            state.sink->on_connectivity(
                {.state = classify_stream(err->code) == exec::ErrorClass::AuthFailure
                              ? exec::ConnState::AuthFailed
                              : exec::ConnState::Connected,
                 .stream_id = k_private_stream_id,
                 .ts = state.clock->wall()});
        }
        break;
    }
    case EventKind::BookTicker:
    case EventKind::Trade:
    case EventKind::Ticker:
    case EventKind::BookUpdate:
    case EventKind::BookSnapshot:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

// Every market this adapter can act on, named exhaustively so Option is a
// case the compiler forces a decision about rather than something a default
// arm swallows. An empty answer here would read as "the venue holds nothing".
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("gate delivery (dated futures) order entry");
    case MarketType::Option:
        // Gate lists options under /api/v4/options, keyed by an underlying and
        // an expiry rather than by a settle currency. Nothing maps an
        // OptionInstrument onto that surface yet.
        return exec::not_implemented("gate options order entry");
    }
    return exec::not_implemented("gate: unknown market");
}

exec::VenueError build_failure(BuildError err) noexcept {
    // A refusal to build is terminal by construction: the same command built
    // again produces the same refusal.
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// One private socket. Futures and spot are different hosts carrying different
// order shapes, so an account trading both holds two of these.
class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        if (!subscribe(orders_channel(market_)) || !subscribe(user_trades_channel(market_))) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, market_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    [[nodiscard]] bool subscribe(std::string_view channel) {
        if (channel.empty() || client_ == nullptr) {
            return false;
        }
        // The user id is positional and futures-only: sending the contract
        // where Gate expects the id subscribes to nothing and reports success.
        std::array<char, 24> id_text{};
        std::array<std::string_view, 2> payload{};
        std::size_t count = 0;
        if (market_ == MarketType::Perp) {
            const auto [stop, ec] = std::to_chars(
                id_text.data(), id_text.data() + id_text.size(), state_->futures_user_id);
            if (ec != std::errc{} || state_->futures_user_id == 0) {
                return false;
            }
            payload[count++] = {id_text.data(), static_cast<std::size_t>(stop - id_text.data())};
        }
        payload[count++] = k_all_symbols;

        const auto frame = state_->protocol.private_subscribe_frame(
            channel, std::span{payload}.first(count), state_->signer, state_->now_s());
        return frame.has_value() && client_->send(*frame);
    }

    VenueState* state_;
    MarketType market_;
    net::WsClient* client_{nullptr};
};

struct PrivateStream {
    PrivateStream(VenueState& state, MarketType market)
        : handler{state, market},
          client{state.io,
                 net::WsConfig{.host = std::string{private_host(state.testnet, market)},
                               .port = std::string{stream_port()},
                               .target = std::string{private_path(state.testnet, market)}},
                 handler} {
        handler.attach(client);
    }

    PrivateHandler handler;
    net::WsClient client;
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class GateExec final : public exec::ExecutionClient {
public:
    // WARNING: Gate names both markets' BTC_USDT identically, and the mappers
    // below do NOT name a market — map_bbo, map_trade, map_order_event and
    // map_user_trade all take (wire, table, ts), and WireOrder carries no
    // market field. So a process with BOTH catalogues loaded would have every
    // one of them answer nullptr and drop every frame, silently. Run one
    // gateway per market; the request builders are already market-correct.
    GateExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          builder_{state_->signer} {
        // Gate's two private streams are different hosts with different order
        // shapes, so exactly one of them opens — the one this process serves.
        if (market == MarketType::Perp) {
            perp_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
        } else if (market == MarketType::Spot) {
            spot_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
        }
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        sync_clock(*state_, rest_);
        if (perp_ && state_->futures_user_id == 0) {
            // Fetched before the socket opens, because on_open cannot ask for
            // it: a subscribe with no user id is answered with an empty stream
            // rather than an error, and that reads as a quiet account.
            if (const auto request = builder_.futures_account(state_->venue_now());
                request.has_value()) {
                if (const auto response = await_response(*state_, rest_, *request);
                    response.has_value() && !response->transport_failed() &&
                    response->status >= 200 && response->status < 300) {
                    if (const auto user = parse_account_user_id(response->body); user.has_value()) {
                        state_->futures_user_id = *user;
                    }
                }
            }
            if (state_->futures_user_id == 0) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::AuthFailure, 0, "gate: futures account id unavailable")};
            }
        }
        if (perp_) {
            perp_->client.start();
        }
        if (spot_) {
            spot_->client.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (perp_) {
            perp_->client.stop();
        }
        if (spot_) {
            spot_->client.stop();
        }
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_ == nullptr || perp_->client.connected();
        const bool spot_up = spot_ == nullptr || spot_->client.connected();
        return started_ && perp_up && spot_up && (perp_ != nullptr || spot_ != nullptr);
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because whether the venue acted is the one
    // thing this process cannot know. Reconciliation closes these.
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            OrderEvent event{};
            event.type = OrderEventType::ReconGap;
            event.instrument = stale.instrument;
            event.client_order_id = stale.client_order_id;
            event.exchange_client_order_id = stale.exchange_client_order_id;
            event.side = stale.side;
            event.qty = stale.qty;
            // UNDEF, not zero: zero leaves would read as "nothing is working",
            // and whether anything is working is exactly what is unknown.
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        // Gate caps a batch at 20 and refuses the whole call past it, so the
        // span is chunked rather than truncated.
        for (std::size_t at = 0; at < cmds.size(); at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(
                at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            const auto request =
                builder_.cancel_batch(chunk, state_->instruments, state_->venue_now());
            if (!request.has_value()) {
                const auto refusal = build_failure(request.error());
                for (const auto& cmd : chunk) {
                    emit_reject(cmd, Kind::Cancel, refusal);
                }
                continue;
            }
            // The per-order verdicts come back in the body; a transport
            // failure is the only thing this callback can honestly report.
            std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
            if (!rest_.send(*request, [this, pending](net::RestResponse&& response) {
                    if (!response.transport_failed() && response.status >= 200 &&
                        response.status < 300) {
                        return;
                    }
                    // A LOST CONNECTION IS WORSE IN A BATCH THAN ALONE: it would declare
                    // every cancel in the chunk refused while the venue may have taken all
                    // of them, leaving the desk sure it still holds orders it does not.
                    if (response.transport_failed()) {
                        for (const auto& cmd : pending) {
                            state_->sink->on_order_error(
                                cmd.client_order_id,
                                exec::VenueError::make(
                                    exec::ErrorClass::Connectivity,
                                    0,
                                    response.error.empty()
                                        ? std::string_view{"no response from venue"}
                                        : std::string_view{response.error}));
                        }
                        return;
                    }
                    const auto refusal = rest_failure(response.body);
                    for (const auto& cmd : pending) {
                        (void)unanswered_.resolve(cmd.client_order_id);
                        emit_reject(cmd, Kind::Cancel, refusal);
                    }
                })) {
                for (const auto& cmd : chunk) {
                    emit_reject(
                        cmd,
                        Kind::Cancel,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            }
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
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
        const auto orders = market == MarketType::Spot ? parse_spot_open_orders(*body, storage)
                                                       : parse_rest_orders(*body, storage, market);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.query_order(id, market, state_->venue_now());
        if (!request.has_value()) {
            // Spot has no client-id lookup that does not also need the pair;
            // the open-order sweep answers the same question.
            return from_open_orders(id, market);
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_rest_order(*body, market);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.my_trades(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local UserTradeBuffer storage;
        const auto trades = parse_rest_user_trades(*body, storage, market);
        if (!trades.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(trades->size());
        for (const auto& wire : *trades) {
            const auto memo = state_->memo_for(wire.order_id);
            const auto event = map_user_trade(wire,
                                              state_->instruments,
                                              memo.leaves,
                                              memo.client_order_id,
                                              state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            reports.push_back({.exec_id = event->exec_id,
                               .exchange_order_id = event->exchange_order_id,
                               .exchange_client_order_id = event->exchange_client_order_id,
                               .instrument = event->instrument,
                               .side = event->side,
                               .price = event->price,
                               .qty = event->qty,
                               .fee = event->fee,
                               .liquidity = event->liquidity,
                               .ts_venue = event->ts_event});
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market != MarketType::Perp) {
            // Gate's spot balances live on a different endpoint with a
            // different shape. Refused rather than answered with the futures
            // account, which would report the wrong wallet under a spot label.
            return std::unexpected{exec::not_implemented("gate spot balance pull")};
        }
        const auto request = builder_.futures_account(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto account = parse_futures_account(*body);
        if (!account.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(account.error()))};
        }
        const auto balance = map_futures_account(*account);
        if (!balance.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "gate: account names no currency")};
        }
        // ONE ROW: the futures account is per settle currency and this gateway
        // trades one. A second settle currency is a second account and a second
        // process, exactly as a second market is.
        return std::vector<BalanceReport>{*balance};
    }

    [[nodiscard]] std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.funding_book(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local AccountBookBuffer storage;
        const auto rows = parse_account_book(*body, storage);
        if (!rows.has_value()) {
            // A body we cannot read is NOT an empty ledger.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& row : *rows) {
            LedgerKind kind{};
            if (row.type == "fund") {
                kind = LedgerKind::Funding;
            } else if (row.type == "fee") {
                kind = LedgerKind::Fee;
            } else {
                // pnl and transfers: derived from fills this process already
                // booked, or not trading money at all.
                continue;
            }
            const auto* instrument = state_->instruments.find(row.contract);
            if (instrument == nullptr) {
                continue;
            }
            const auto amount = parse_decimal_e9(row.change);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero one
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            charge.amount = *amount;
            charge.kind = kind;
            // SECONDS on this endpoint, not milliseconds.
            charge.settled_at = UnixNanos{row.time_s * 1'000'000'000ULL};
            // A fee names the trade it was charged on, and that is what the
            // comparison keys on; the book's own row id is the fallback.
            const auto reference =
                kind == LedgerKind::Fee && !row.trade_id.empty() ? row.trade_id : row.id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    [[nodiscard]] std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot balance is not a position. Answering "flat" would be a
            // claim about a concept Gate does not have on this API.
            return std::unexpected{exec::not_implemented("gate spot position pull")};
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

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request) {
        const auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body)};
        }
        return response->body;
    }

    [[nodiscard]] std::expected<exec::OrderStatusReport, exec::VenueError>
    from_open_orders(const ExchangeClientOrderId& id, MarketType market) {
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

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        const auto market = cmd.instrument.market();
        if (!rest_.send(*request, [this, cmd, kind, market](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind, market);
            })) {
            // Never left this process, so nothing is in doubt.
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    void
    on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind, MarketType market) {
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(cmd.client_order_id);
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            state_->sink->on_order_error(
                cmd.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        if (response.status >= 200 && response.status < 300) {
            // The success body IS the order. Remembering it here seeds the
            // leaves before the socket's `_new` push arrives, which is what
            // lets a fill that overtakes that push still state its leaves.
            if (const auto wire = parse_rest_order(response.body, market); wire.has_value()) {
                state_->remember(*wire);
            }
            return;
        }
        if (kind == Kind::Cancel) {
            if (const auto err = parse_rest_error(response.body);
                err.has_value() && is_benign_cancel_failure(err->label)) {
                return; // the order already finished; reconciliation settles it
            }
        }
        emit_reject(cmd, kind, rest_failure(response.body));
    }

    // The reason is not optional. `OrderEvent` has no room for text, so every
    // refusal hands the sink the verdict that caused it — otherwise an order
    // that dies here is indistinguishable from one that was never sent.
    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
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
        event.qty = cmd.qty;
        event.price = cmd.price;
        // A create that never reached the book leaves nothing behind; a
        // refused cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> perp_;
    std::unique_ptr<PrivateStream> spot_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<GateExec>(std::move(state), market);
}

} // namespace raikiri::venues::gate::detail
