#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::okx::detail {

namespace {

void handle_order_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local OrderBuffer storage;
    const auto orders = parse_orders(frame, storage);
    if (!orders.has_value()) {
        return;
    }
    for (const auto& wire : *orders) {
        // The fill goes first. One message is both the last trade and the
        // cancel that ended the order, and publishing the terminal event
        // ahead of the money moves an order to Canceled with a fill still
        // unaccounted for.
        if (const auto fill = map_order_fill(wire, state.instruments, received); fill.has_value()) {
            state.sink->on_order_event(*fill);
        }
        if (const auto event = map_order_event(wire, state.instruments, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
}

// Private order socket: login first, subscribe only once OKX answers it.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto login = state_->private_protocol.login_frame(
            state_->signer, state_->passphrase.view(), state_->venue_now());
        if (!login.has_value() || !client_->send(*login)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            return;
        }
        // No subscribe here on purpose: a private channel asked for before the
        // login answer lands is refused with 60011, and the refusal looks
        // exactly like a channel that has nothing to say.
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // the bare "pong", and anything OKX adds later
        }
        switch (event->kind) {
        case EventKind::LoginAck: {
            const auto ack = parse_stream_error(payload);
            const bool authenticated = ack.has_value() && ack->code == k_ok;
            if (!authenticated) {
                state_->last_error_code = ack.has_value() ? ack->code : 0;
            }
            state_->sink->on_connectivity({.state = authenticated ? exec::ConnState::Resubscribed
                                                                  : exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            if (authenticated) {
                subscribe_orders();
            }
            break;
        }
        case EventKind::Orders:
            handle_order_frame(*state_, payload, received);
            break;
        case EventKind::Error:
            if (const auto err = parse_stream_error(payload); err.has_value()) {
                state_->last_error_code = err->code;
                ++state_->subscribe_refusals;
            }
            break;
        case EventKind::Bbo:
        case EventKind::Trade:
        case EventKind::FundingRate:
        case EventKind::MarkPrice:
        case EventKind::Tickers:
        case EventKind::Books:
        case EventKind::SubscribeAck:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    void subscribe_orders() {
        // OKX offers a dedicated `fills` channel, but it answers 64003 below
        // trading fee tier VIP4 and its own docs send everyone else here: the
        // orders channel carries fillPx/fillSz/tradeId on every trade, so it
        // is the fill feed as well as the lifecycle feed.
        std::array<Subscription, 2> args{};
        std::size_t count = 0;
        if (state_->spot_enabled) {
            args[count++] = {.channel = k_orders_channel,
                             .inst_type = inst_type_of(MarketType::Spot)};
        }
        if (state_->perp_enabled) {
            args[count++] = {.channel = k_orders_channel,
                             .inst_type = inst_type_of(MarketType::Perp)};
        }
        if (count == 0) {
            return;
        }
        const auto frame = state_->private_protocol.subscribe_frame(
            std::span<const Subscription>{args.data(), count});
        if (frame.has_value()) {
            (void)client_->send(frame->text);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class OkxExec final : public exec::ExecutionClient {
public:
    explicit OkxExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          ws_{state_->io,
              net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                            .port = std::string{stream_port()},
                            .target = std::string{private_stream_path()}},
              handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(ws_);
        builder_.set_passphrase(state_->passphrase.view());
        builder_.set_simulated(state_->testnet);
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        sync_clock(*state_, rest_);
        if (!state_->credentialed()) {
            // Market data still runs; every order path below refuses, so
            // nothing can sit open waiting for a verdict no code will produce.
            return {};
        }
        ws_.start();
        return {};
    }

    void stop() override {
        ws_.stop();
        rest_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return ws_.connected(); }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — and closed by reconciliation.
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
            // UNDEF, not zero: zero leaves would read as "nothing is working".
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override {
        fire(builder_.create_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        fire(builder_.cancel_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        fire(builder_.amend_order(cmd, state_->instruments, state_->venue_now()),
             cmd,
             OrderEventType::ModifyRejected);
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto batch =
                builder_.cancel_batch(cmds.subspan(sent), state_->instruments, state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                break;
            }
            const auto chunk = cmds.subspan(sent, batch->consumed);
            if (!send_batch(batch->request, chunk)) {
                break;
            }
            sent += batch->consumed;
        }
        // Whatever the builder would not take — an instrument the catalogue
        // does not know, a wedged queue — is refused out loud. A cancel that
        // vanishes leaves an order live that the strategy believes is gone.
        for (const auto& cmd : cmds.subspan(sent)) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(
                     exec::ErrorClass::Terminal, 0, "batch would not take this cancel"));
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
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
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
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

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
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

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
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

private:
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

    void fire(const std::expected<net::RestRequest, BuildError>& request,
              const OrderCmd& cmd,
              OrderEventType on_failure) noexcept {
        if (!request.has_value()) {
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const auto captured = cmd;
        const auto sent = rest_.send(*request, [this, captured, on_failure](net::RestResponse&& r) {
            const auto refusal = verdict(r, captured.exchange_client_order_id, on_failure);
            // A CONNECTIVITY REFUSAL IS NOT THE VENUE'S ANSWER. The request may
            // have reached the matcher before the read failed, so it stays in
            // the table for the sweep rather than being called rejected here.
            if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                state_->sink->on_order_error(captured.client_order_id, *refusal);
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            if (refusal.has_value()) {
                emit(captured, on_failure, *refusal);
            }
        });
        if (!sent) {
            // Never left this process, so nothing is in doubt.
            emit(cmd,
                 on_failure,
                 exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // ON THE WIRE AND UNANSWERED, recorded before the callback that may
        // never run.
        (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                               .exchange_client_order_id = cmd.exchange_client_order_id,
                               .instrument = cmd.instrument,
                               .side = cmd.side,
                               .qty = cmd.qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    bool send_batch(const net::RestRequest& request, std::span<const OrderCmd> chunk) noexcept {
        std::vector<OrderCmd> captured{chunk.begin(), chunk.end()};
        const bool sent = rest_.send(request, [this, captured](net::RestResponse&& r) {
            for (const auto& cmd : captured) {
                const auto refusal =
                    verdict(r, cmd.exchange_client_order_id, OrderEventType::CancelRejected);
                // WORSE IN A BATCH THAN ALONE. A lost connection here would
                // declare every cancel in the chunk refused, while the venue may
                // have taken all of them — leaving the desk sure it still has
                // orders it no longer has.
                if (refusal.has_value() && refusal->cls == exec::ErrorClass::Connectivity) {
                    state_->sink->on_order_error(cmd.client_order_id, *refusal);
                    continue;
                }
                (void)unanswered_.resolve(cmd.client_order_id);
                if (refusal.has_value()) {
                    emit(cmd, OrderEventType::CancelRejected, *refusal);
                }
            }
        });
        if (!sent) {
            return false;
        }
        for (const auto& cmd : chunk) {
            (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                                   .exchange_client_order_id = cmd.exchange_client_order_id,
                                   .instrument = cmd.instrument,
                                   .side = cmd.side,
                                   .qty = cmd.qty,
                                   .kind = cmd.kind,
                                   .sent = state_->clock->mono()});
        }
        return true;
    }

    // OKX answers a refused single order with envelope code "1" — "Operation
    // failed" — and puts the reason in data[].sCode. Reading only the
    // envelope turns every rejection into the same unclassifiable failure,
    // and reading only sCode misses a request that never reached the matcher.
    //
    // ABSENT MEANS ACCEPTED. The refusal carries its own reason rather than a
    // bare verdict, because sCode is the only place the reason exists and a
    // caller that got back an enum has already lost it.
    [[nodiscard]] std::optional<exec::VenueError> verdict(const net::RestResponse& response,
                                                          const ExchangeClientOrderId& id,
                                                          OrderEventType on_failure) const {
        if (response.transport_failed()) {
            return exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response from venue");
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope");
        }
        static thread_local AckBuffer storage;
        const auto acks = parse_order_acks(response.body, storage);
        std::int64_t code = envelope->code;
        std::string_view text = envelope->msg;
        if (acks.has_value()) {
            for (const auto& ack : *acks) {
                // A batch answers for several orders at once, so the entry
                // that matters is the one carrying this order's id.
                if (ack.cl_ord_id == id.view() || acks->size() == 1) {
                    code = ack.s_code;
                    text = ack.s_msg;
                    break;
                }
            }
        }
        if (code == k_ok) {
            return std::nullopt; // the private stream reports what follows
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(code)) {
            return std::nullopt; // the order already filled or was gone
        }
        if (on_failure == OrderEventType::ModifyRejected && is_benign_amend_failure(code)) {
            return std::nullopt;
        }
        return exec::VenueError::make(classify(code), code, text);
    }

    // The reason is not optional. `OrderEvent` has no room for text, so every
    // refusal hands the sink the verdict that caused it — otherwise an order
    // that dies here is indistinguishable from one that was never sent.
    void emit(const OrderCmd& cmd, OrderEventType type, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = type;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        event.leaves_qty = type == OrderEventType::Rejected ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient ws_;
    net::RestClient rest_;
    RequestBuilder builder_;
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OkxExec>(std::move(state));
}

} // namespace raikiri::venues::okx::detail
