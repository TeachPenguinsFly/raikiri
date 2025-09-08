#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/xt/client_id.hpp>
#include <raikiri/venues/xt/endpoints.hpp>
#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/venues/xt/map.hpp>
#include <raikiri/venues/xt/rest.hpp>
#include <raikiri/venues/xt/ws.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::xt::detail {

namespace {

// XT closes a private socket that has sent no application `ping` for 30 s, and
// only CLIENT messages count toward that timer — a connection receiving order
// pushes the whole time is still dropped. Ten seconds of headroom leaves room
// for a missed tick and a reconnect inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

constexpr std::uint32_t k_perp_stream_id = 1;

constexpr std::uint32_t k_spot_stream_id = 2;

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Answered BEFORE the instrument table is read: the table compares
// the WHOLE id, so an id for a market that was never loaded resolves to nothing
// and would otherwise be reported as an unknown instrument — blaming a stale
// catalogue for a market this adapter simply does not serve.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        return exec::not_implemented("xt dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("xt options order entry");
    }
    return exec::not_implemented("xt: unknown market");
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. Missing credentials are the one exception worth
// separating — that sends an operator to the config rather than to XT's status
// page.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// The two trees report a refusal in disjoint vocabularies and disjoint
// envelopes, and spot does it at HTTP 200 — so the status is never the verdict
// there, and reading one tree's body with the other's parser loses the code
// that decides retry from halt.
exec::VenueError rest_failure(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto envelope = parse_spot_envelope(body);
        if (!envelope.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
        }
        return exec::VenueError::make(
            classify_spot(envelope->mc), numeric_code(envelope->mc), envelope->mc);
    }
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    return exec::VenueError::make(
        classify(envelope->code), numeric_code(envelope->code), envelope->msg);
}

// True when the body says the call succeeded. Spot answers HTTP 200 to a
// refusal, so `rc` is the only thing that distinguishes them; futures answers
// a non-2xx status AND returnCode 1, and either alone is enough to fail.
bool envelope_ok(std::string_view body, MarketType market) {
    if (market == MarketType::Spot) {
        const auto envelope = parse_spot_envelope(body);
        return envelope.has_value() && envelope->rc == k_spot_ok;
    }
    const auto envelope = parse_rest_envelope(body);
    return envelope.has_value() && envelope->return_code == k_ok;
}

// Routes one private push. Both trees wear the same {topic,event,data} shell
// and NOTHING ELSE in common: futures states an order with long field names,
// spot with two-letter ones, so the reader is chosen by the market rather than
// sniffed from the payload.
void handle_private_frame(VenueState& state,
                          MarketType market,
                          std::string_view frame,
                          UnixNanos received) {
    const auto info = parse_private_frame(frame);
    if (!info.has_value()) {
        return; // the `pong` answer is bare text, not JSON
    }
    const auto stream_id = market == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
    switch (info->topic) {
    case PrivateTopic::Order: {
        if (market == MarketType::Spot) {
            const auto wire = parse_spot_order_push(frame);
            if (!wire.has_value()) {
                return;
            }
            // Remembered BEFORE the event is emitted: a fill for the same order
            // may already be queued behind this frame, and it can state neither
            // its side nor its leaves out of anything else.
            OrderContext context = state.context_for(wire->order_id);
            context.side = wire->side == "SELL" ? Side::Sell : Side::Buy;
            context.side_known = !wire->side.empty();
            if (const auto report = map_spot_open_order(*wire, state.instruments);
                report.has_value()) {
                context.leaves = report->leaves;
                if (!report->exchange_client_order_id.empty()) {
                    context.client_order_id = report->exchange_client_order_id;
                }
            }
            state.remember(wire->order_id, context);
            if (const auto event = map_spot_order_event(*wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
            return;
        }
        const auto wire = parse_futures_order(frame);
        if (!wire.has_value()) {
            return;
        }
        OrderContext context = state.context_for(wire->order_id);
        context.side = wire->order_side == "SELL" ? Side::Sell : Side::Buy;
        context.side_known = !wire->order_side.empty();
        if (const auto report = map_futures_open_order(*wire, state.instruments);
            report.has_value()) {
            context.leaves = report->leaves;
            if (!report->exchange_client_order_id.empty()) {
                context.client_order_id = report->exchange_client_order_id;
            }
        }
        state.remember(wire->order_id, context);
        if (const auto event = map_futures_order_event(
                *wire, state.instruments, context.client_order_id, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    case PrivateTopic::Trade: {
        if (market == MarketType::Spot) {
            const auto wire = parse_spot_trade_push(frame);
            if (!wire.has_value()) {
                return;
            }
            if (const auto event = map_spot_trade(
                    *wire, state.instruments, state.context_for(wire->order_id), received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
            return;
        }
        const auto wire = parse_futures_deal(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto event = map_futures_deal(
                *wire, state.instruments, state.context_for(wire->order_id), received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
        return;
    }
    case PrivateTopic::Position:
    case PrivateTopic::Balance:
        // GatewaySink carries no channel for either; the reconciler asks for
        // positions through position_reports() instead.
        return;
    case PrivateTopic::Ack:
        state.sink->on_connectivity({.state = exec::ConnState::Connected,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateTopic::Refusal:
        // Code 2 is the only refusal with a remedy: the key is dead and a new
        // one has to be minted before the socket can carry anything.
        if (info->code == k_listen_key_invalid) {
            (market == MarketType::Spot ? state.spot_key_stale : state.perp_key_stale) = true;
        }
        state.last_error.assign(info->msg);
        state.sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                     .stream_id = stream_id,
                                     .ts = state.clock->wall()});
        return;
    case PrivateTopic::Unknown:
        return;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    PrivateHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        const auto stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id;
        const auto frame = market_ == MarketType::Spot ? protocol_.spot_subscribe(*key_)
                                                       : protocol_.futures_subscribe(*key_);
        if (!frame.has_value() || client_ == nullptr || !client_->send(*frame)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, market_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected,
             .stream_id = market_ == MarketType::Spot ? k_spot_stream_id : k_perp_stream_id,
             .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client, const std::string& key) noexcept {
        client_ = &client;
        key_ = &key;
    }

private:
    VenueState* state_;
    MarketType market_;
    PrivateProtocol protocol_;
    net::WsClient* client_{nullptr};
    const std::string* key_{nullptr};
};

// One private connection. The key is owned here rather than by the handler
// because it is re-minted over REST between reconnects.
struct PrivateStream {
    PrivateStream(VenueState& state, MarketType market)
        : handler{state, market},
          client{state.io,
                 net::WsConfig{
                     .host = std::string{market == MarketType::Spot ? spot_private_stream_host()
                                                                    : private_stream_host()},
                     .port = "443",
                     .target = std::string{market == MarketType::Spot ? spot_private_stream_path()
                                                                      : private_stream_path()}},
                 handler} {
        handler.attach(client, key);
    }

    PrivateHandler handler;
    net::WsClient client;
    std::string key;
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Order entry over REST for both markets, with the lifecycle arriving on the
// matching private socket. The REST answer to a create carries only the venue
// order id — the socket's `NEW` push is the acceptance — so nothing is emitted
// on a success here beyond seeding the memo that later fills read their side
// and leaves out of.
class XtExec final : public exec::ExecutionClient {
public:
    XtExec(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          spot_rest_{state_->io,
                     net::RestConfig{.host = std::string{spot_rest_host()}, .port = "443"}},
          builder_{state_->signer}, spot_{market == MarketType::Spot},
          perp_{market == MarketType::Perp} {}

    ~XtExec() override { state_->private_poll = nullptr; }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        if (perp_) {
            sync_clock(*state_, rest_, unsigned_builder_);
            perp_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Perp);
            if (mint_listen_key()) {
                perp_stream_->client.start();
            }
        }
        if (spot_) {
            spot_rest_.start();
            sync_spot_clock(*state_, spot_rest_, unsigned_builder_);
            spot_stream_ = std::make_unique<PrivateStream>(*state_, MarketType::Spot);
            if (mint_ws_token()) {
                spot_stream_->client.start();
            }
        }
        state_->private_poll = [this](UnixNanos now) { poll_private(now); };
        started_ = true;
        return {};
    }

    void stop() override {
        state_->private_poll = nullptr;
        if (perp_stream_) {
            perp_stream_->client.stop();
        }
        if (spot_stream_) {
            spot_stream_->client.stop();
        }
        rest_.stop();
        spot_rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        const bool perp_up = perp_stream_ == nullptr || perp_stream_->client.connected();
        const bool spot_up = spot_stream_ == nullptr || spot_stream_->client.connected();
        return started_ && perp_up && spot_up && (spot_ || perp_);
    }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        spot_rest_.keepalive(now);
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

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto request = builder_.open_orders(market, {}, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<exec::OrderStatusReport> reports;
        if (market == MarketType::Spot) {
            static thread_local SpotOrderBuffer storage;
            const auto orders = parse_spot_orders(*body, storage);
            if (!orders.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
            }
            reports.reserve(orders->size());
            for (const auto& wire : *orders) {
                if (auto report = map_spot_open_order(wire, state_->instruments);
                    report.has_value()) {
                    seed(wire.order_id, *report);
                    reports.push_back(*report);
                }
            }
            return reports;
        }
        static thread_local FuturesOrderBuffer storage;
        const auto orders = parse_futures_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_futures_open_order(wire, state_->instruments);
                report.has_value()) {
                seed(wire.order_id, *report);
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
        if (market != MarketType::Spot) {
            // /future/trade/v1/order/detail is keyed by the VENUE order id and
            // XT publishes no by-client-id read, so the open-order sweep is
            // what can answer this question at all.
            return from_open_orders(id, market);
        }
        const auto request = builder_.query_order(id, market, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_spot_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        auto report = map_spot_open_order(*wire, state_->instruments);
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
        const auto request = builder_.my_trades(market, since, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::vector<exec::FillReport> reports;
        const auto push = [&reports](const OrderEvent& event) {
            reports.push_back({.exec_id = event.exec_id,
                               .exchange_order_id = event.exchange_order_id,
                               .exchange_client_order_id = event.exchange_client_order_id,
                               .instrument = event.instrument,
                               .side = event.side,
                               .price = event.price,
                               .qty = event.qty,
                               .fee = event.fee,
                               .liquidity = event.liquidity,
                               .ts_venue = event.ts_event});
        };
        if (market == MarketType::Spot) {
            static thread_local SpotTradeBuffer storage;
            const auto trades = parse_spot_trades(*body, storage);
            if (!trades.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
            }
            reports.reserve(trades->size());
            for (const auto& wire : *trades) {
                if (const auto event = map_spot_trade(
                        wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
                    event.has_value()) {
                    push(*event);
                }
            }
            return reports;
        }
        static thread_local FuturesDealBuffer storage;
        const auto deals = parse_futures_deals(*body, storage);
        if (!deals.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(deals.error()))};
        }
        reports.reserve(deals->size());
        for (const auto& wire : *deals) {
            if (const auto event = map_futures_deal(
                    wire, state_->instruments, state_->context_for(wire.order_id), UnixNanos{});
                event.has_value()) {
                push(*event);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // The spot wallet answers from the other host, under the other
            // envelope, with none of the field names below. Refused rather than
            // answered with the futures wallet, which would report the wrong
            // money under a spot label.
            return std::unexpected{exec::not_implemented("xt spot balance pull")};
        }
        const auto request = builder_.futures_balances(state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FuturesBalanceBuffer storage;
        const auto balances = parse_futures_balances(*body, storage);
        if (!balances.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(balances.error()))};
        }
        std::vector<BalanceReport> reports;
        reports.reserve(balances->size());
        for (const auto& wire : *balances) {
            // A row XT named no coin for is dropped rather than emitted under an
            // empty asset code.
            if (auto report = map_futures_balance(wire); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (market == MarketType::Spot) {
            // A spot balance is not a position. Answering "flat" would be a
            // claim about a concept this API does not have.
            return std::unexpected{exec::not_implemented("xt spot position pull")};
        }
        const auto request = builder_.positions(market, state_->venue_now(market));
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request, market);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FuturesPositionBuffer storage;
        const auto positions = parse_futures_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_futures_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] net::RestClient& rest_for(MarketType market) noexcept {
        return market == MarketType::Spot ? spot_rest_ : rest_;
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    fetch(const net::RestRequest& request, MarketType market) {
        const auto response = await_response(*state_, rest_for(market), request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected{rest_failure(response->body, market)};
        }
        // Spot answers a refusal at HTTP 200, so the status alone is never the
        // verdict on that tree.
        if (!envelope_ok(response->body, market)) {
            return std::unexpected{rest_failure(response->body, market)};
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

    void seed(std::string_view order_id, const exec::OrderStatusReport& report) {
        state_->remember(order_id,
                         {.client_order_id = report.exchange_client_order_id,
                          .leaves = report.leaves,
                          .side = report.side,
                          .side_known = true});
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto market = cmd.instrument.market();
        const auto now = state_->venue_now(market);
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!rest_for(market).send(*request,
                                   [this, cmd, kind, market](net::RestResponse&& response) {
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
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (http_ok && envelope_ok(response.body, market)) {
            if (kind == Kind::Create) {
                // The ack carries the venue order id and nothing else. Seeding
                // the memo here is what lets a fill that overtakes the socket's
                // NEW push still state its side and its leaves.
                if (const auto id = parse_order_ack(response.body); id.has_value()) {
                    state_->remember(*id,
                                     {.client_order_id = cmd.exchange_client_order_id,
                                      .leaves = cmd.qty,
                                      .side = cmd.side,
                                      .side_known = true});
                }
            }
            return; // the socket's push carries the lifecycle
        }
        if (kind == Kind::Cancel) {
            const auto code =
                market == MarketType::Spot
                    ? parse_spot_envelope(response.body).transform([](const SpotEnvelope& e) {
                          return e.mc;
                      })
                    : parse_rest_envelope(response.body).transform([](const RestEnvelope& e) {
                          return e.code;
                      });
            if (code.has_value() && is_benign_cancel_failure(*code)) {
                return; // the order already finished; reconciliation settles it
            }
        }
        emit_reject(cmd, kind, rest_failure(response.body, market));
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
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // Minted synchronously, and only at start(): the socket cannot subscribe
    // without a key, and a cold start is the one place a blocking round trip
    // costs nothing. A FAILURE MARKS THE KEY STALE so the poll below re-mints
    // it without ever blocking the gateway loop.
    [[nodiscard]] bool mint_listen_key() {
        const auto request = builder_.listen_key(state_->venue_now(MarketType::Perp));
        const auto key =
            request.has_value()
                ? fetch(*request, MarketType::Perp).and_then([](const std::string& body) {
                      const auto parsed = parse_listen_key(body);
                      return parsed.has_value()
                                 ? std::expected<std::string, exec::VenueError>{std::string{
                                       *parsed}}
                                 : std::unexpected{exec::VenueError::make(
                                       exec::ErrorClass::Terminal, 0, "no key")};
                  })
                : std::unexpected{build_failure(request.error())};
        state_->perp_key_stale = !key.has_value();
        if (!key.has_value()) {
            return false;
        }
        perp_stream_->key = *key;
        return true;
    }

    [[nodiscard]] bool mint_ws_token() {
        const auto request = builder_.ws_token(state_->venue_now(MarketType::Spot));
        const auto token =
            request.has_value()
                ? fetch(*request, MarketType::Spot).and_then([](const std::string& body) {
                      const auto parsed = parse_spot_ws_token(body);
                      return parsed.has_value()
                                 ? std::expected<std::string, exec::VenueError>{std::string{
                                       *parsed}}
                                 : std::unexpected{exec::VenueError::make(
                                       exec::ErrorClass::Terminal, 0, "no token")};
                  })
                : std::unexpected{build_failure(request.error())};
        state_->spot_key_stale = !token.has_value();
        if (!token.has_value()) {
            return false;
        }
        spot_stream_->key = *token;
        return true;
    }

    // A key XT declared invalid is re-minted ASYNCHRONOUSLY: this runs on the
    // gateway's poll, and the blocking fetch() that start() can afford would
    // stall every other venue for the length of one XT round trip.
    void refresh_key(MarketType market) noexcept {
        auto* stream = market == MarketType::Spot ? spot_stream_.get() : perp_stream_.get();
        bool& in_flight = market == MarketType::Spot ? spot_key_in_flight_ : perp_key_in_flight_;
        if (stream == nullptr || in_flight) {
            return;
        }
        const auto now = state_->venue_now(market);
        const auto request =
            market == MarketType::Spot ? builder_.ws_token(now) : builder_.listen_key(now);
        if (!request.has_value()) {
            return; // no credentials: nothing to retry with
        }
        in_flight = true;
        if (!rest_for(market).send(*request, [this, market](net::RestResponse&& response) {
                on_key(std::move(response), market);
            })) {
            in_flight = false;
        }
    }

    void on_key(net::RestResponse&& response, MarketType market) {
        auto* stream = market == MarketType::Spot ? spot_stream_.get() : perp_stream_.get();
        (market == MarketType::Spot ? spot_key_in_flight_ : perp_key_in_flight_) = false;
        if (stream == nullptr || response.transport_failed() ||
            !envelope_ok(response.body, market)) {
            return; // still stale; the next poll tries again
        }
        const auto key = market == MarketType::Spot ? parse_spot_ws_token(response.body)
                                                    : parse_listen_key(response.body);
        if (!key.has_value()) {
            return;
        }
        stream->key.assign(*key);
        (market == MarketType::Spot ? state_->spot_key_stale : state_->perp_key_stale) = false;
        // The socket subscribes with whatever key it holds AT on_open, so a
        // fresh key only takes effect on a fresh connection.
        stream->client.stop();
        stream->client.start();
    }

    // Driven by the data client's poll: ExecutionClient has no hook of its own
    // and XT's 30 s idle timer counts only what the client sent.
    void poll_private(UnixNanos now) noexcept {
        if (state_->perp_key_stale) {
            refresh_key(MarketType::Perp);
        }
        if (state_->spot_key_stale) {
            refresh_key(MarketType::Spot);
        }
        if (now.ns < next_ping_ns_) {
            return;
        }
        if (perp_stream_ && perp_stream_->client.connected()) {
            (void)perp_stream_->client.send(PrivateProtocol::ping_frame());
        }
        if (spot_stream_ && spot_stream_->client.connected()) {
            (void)spot_stream_->client.send(PrivateProtocol::ping_frame());
        }
        next_ping_ns_ = now.ns + k_app_ping_interval_ns;
    }

    std::shared_ptr<VenueState> state_;
    std::unique_ptr<PrivateStream> perp_stream_;
    std::unique_ptr<PrivateStream> spot_stream_;
    net::RestClient rest_;
    net::RestClient spot_rest_;
    RequestBuilder unsigned_builder_;
    SignedRequestBuilder builder_;
    std::uint64_t next_ping_ns_{0};
    bool perp_key_in_flight_{false};
    bool spot_key_in_flight_{false};
    bool spot_{false};
    bool perp_{false};
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<XtExec>(std::move(state), market);
}

} // namespace raikiri::venues::xt::detail
