#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitmex::detail {

namespace {

// The private socket is its own connection, so it reports its own health.
constexpr std::uint32_t k_private_stream_id = 1;

// The body may not parse; the status classifies either way.
exec::VenueError rest_failure(const net::RestResponse& response) {
    const auto detail = parse_rest_error(response.body);
    const auto name = detail.has_value() ? detail->name : std::string_view{};
    const auto message = detail.has_value() ? detail->message : std::string_view{"http"};
    return exec::VenueError::make(classify(response.status, name), response.status, message);
}

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Named BEFORE the instrument table is read: InstrumentTable::find
// compares the whole id, so an id for a market we never loaded resolves to
// nothing and would otherwise be reported as an unknown instrument.
exec::VenueError build_failure(BuildError err) noexcept {
    switch (err) {
    case BuildError::DatedFuturesUnsupported:
        return exec::not_implemented("bitmex dated futures order entry");
    case BuildError::OptionsUnsupported:
        return exec::not_implemented("bitmex options order entry");
    case BuildError::NotSigned:
        // Misconfigured, not broken: the reconciler retries once an operator
        // fixes it, and halting the venue as Terminal is the wrong response.
        return exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, to_string(err));
    case BuildError::UnknownInstrument:
    case BuildError::BadQuantity:
    case BuildError::BadPrice:
    case BuildError::BufferTooSmall:
    case BuildError::NoOrderTarget:
    case BuildError::EmptyBatch:
    case BuildError::BatchTooLarge:
        break;
    }
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    return refusal.has_value() ? std::optional{build_failure(*refusal)} : std::nullopt;
}

OrderEventType event_type_of(OrderEffect effect) noexcept {
    switch (effect) {
    case OrderEffect::Canceled:
        return OrderEventType::Canceled;
    case OrderEffect::Rejected:
        return OrderEventType::Rejected;
    case OrderEffect::Expired:
        return OrderEventType::Expired;
    case OrderEffect::Accepted:
    case OrderEffect::Silent:
        break;
    }
    return OrderEventType::Accepted;
}

bool is_terminal(OrderEffect effect) noexcept {
    return effect == OrderEffect::Canceled || effect == OrderEffect::Rejected ||
           effect == OrderEffect::Expired;
}

// Merges one `order` row onto what this process remembered about that order and
// publishes the lifecycle event if — and only if — the status actually moved.
//
// Both the REST responses and the private socket come through here, which is
// what makes the two agree: whichever arrives first publishes, and the second
// finds `last_effect` already set and stays quiet. Without that a create whose
// response and whose stream update both said "New" would emit Accepted twice,
// and the OMS transition table treats the second as a protocol violation.
void publish_order(VenueState& state,
                   const WireOrder& wire,
                   FrameAction action,
                   UnixNanos received) {
    if (wire.order_id.empty()) {
        return;
    }
    const std::string key{wire.order_id};
    auto* memo = state.recall(key);
    const auto* instrument =
        wire.symbol.empty() ? (memo != nullptr ? state.instruments.find(memo->instrument) : nullptr)
                            : state.instruments.find(wire.symbol);
    if (instrument == nullptr) {
        // An `update` names an order id and an ordStatus and nothing else, so
        // an order this process never saw created has no instrument to
        // attribute it to. An event on the wrong instrument is worse than one
        // that never fires; reconciliation is what closes the gap.
        return;
    }
    const auto& base = base_of(*instrument);
    const auto columns = map_order_columns(wire, base, received);
    if (!columns.has_value()) {
        return; // a column that IS present failed to convert: a schema event
    }

    LiveOrder merged = memo != nullptr ? *memo : LiveOrder{};
    merged.instrument = base.id;
    if (columns->side.has_value()) {
        merged.side = *columns->side;
    }
    if (columns->order_type.has_value()) {
        merged.order_type = *columns->order_type;
    }
    if (columns->flags.has_value()) {
        merged.flags = *columns->flags;
    }
    if (columns->price.has_value()) {
        merged.price = *columns->price;
    }
    if (columns->qty.has_value()) {
        merged.qty = *columns->qty;
    }
    if (columns->leaves.has_value()) {
        merged.leaves = *columns->leaves;
    }
    if (!wire.cl_ord_id.empty()) {
        if (const auto id = ExchangeClientOrderId::from(wire.cl_ord_id); id.has_value()) {
            merged.client_order_id = *id;
        }
    }

    const auto effect = wire.ord_status.empty() ? OrderEffect::Silent : effect_of(wire.ord_status);
    // A `partial` is the working set at connect time, not news: the OMS already
    // knows these orders, and replaying them as fresh acknowledgements is what
    // reconciliation exists to avoid.
    const bool publish = action != FrameAction::Partial && effect != OrderEffect::Silent &&
                         effect != merged.last_effect;
    if (effect != OrderEffect::Silent) {
        merged.last_effect = effect;
    }
    state.remember(key, merged);

    if (publish) {
        OrderEvent event{};
        event.type = event_type_of(effect);
        event.instrument = merged.instrument;
        event.side = merged.side;
        event.order_type = merged.order_type;
        event.flags = merged.flags;
        event.exchange_client_order_id = merged.client_order_id;
        if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = merged.price;
        event.qty = merged.qty;
        event.leaves_qty = is_terminal(effect) ? QtyLots::from_raw(0) : merged.leaves;
        event.ts_event = columns->ts_event;
        event.ts_init = received;
        state.sink->on_order_event(event);
    }
    // Nothing more will be said about a finished order, and the memo is bounded.
    if (is_terminal(effect) || wire.ord_status == "Filled") {
        state.live_orders.erase(key);
    }
}

void publish_executions(VenueState& state, std::string_view payload, UnixNanos received) {
    static thread_local ExecutionBuffer storage;
    const auto rows = parse_executions(payload, storage);
    if (!rows.has_value()) {
        return;
    }
    for (const auto& wire : *rows) {
        // Funding and Settlement rows carry a price and a quantity too; only
        // execType "Trade" is a fill, and map_execution is where that is read.
        const auto event = map_execution(wire, state.instruments, received);
        if (!event.has_value()) {
            continue;
        }
        if (auto* memo = state.recall(std::string{wire.order_id}); memo != nullptr) {
            memo->leaves = event->leaves_qty;
        }
        state.sink->on_order_event(*event);
    }
}

// BitMEX's private tables ride the same endpoint as the public ones but behind
// a signed handshake, so this is a second connection: a market-data reconnect
// must not have to re-authenticate, and an order stream must not drop because a
// book resubscribe failed.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    void on_open() override {
        const auto expires =
            state_->clock->wall().ns / 1'000'000'000ULL + SignedRequestBuilder::k_expiry_window_s;
        std::array<char, 64> message{};
        const auto text = auth_message(expires, message);
        std::array<char, net::k_hex_bytes> digest{};
        const auto signature = text.empty()
                                   ? std::string_view{}
                                   : state_->signer.sign(text, net::Signer::Encoding::Hex, digest);
        const auto frame = protocol_.auth_frame(state_->signer.api_key(), expires, signature);
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        // Sent without waiting for an ack on purpose: BitMEX processes a
        // connection's messages in order, and the refusal path is an error
        // frame rather than a silent drop of the subscribe.
        const std::array<std::string_view, 3> tables{
            k_table_order, k_table_execution, k_table_position};
        const auto subscribe = protocol_.private_subscribe_frame(tables);
        if (!subscribe.has_value() || !client_->send(subscribe->text)) {
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
        const auto info = parse_frame(payload);
        if (!info.has_value()) {
            return; // "pong" is not JSON, and neither is a frame BitMEX adds
        }
        switch (info->table) {
        case TableKind::Order: {
            static thread_local OrderBuffer storage;
            const auto rows = parse_orders(payload, storage);
            if (!rows.has_value()) {
                return;
            }
            for (const auto& wire : *rows) {
                publish_order(*state_, wire, info->action, received);
            }
            return;
        }
        case TableKind::Execution:
            publish_executions(*state_, payload, received);
            return;
        case TableKind::Position:
            // Positions are pulled, not streamed onward: GatewaySink carries no
            // position channel, and a reconciler asks for the whole picture.
            return;
        case TableKind::Error: {
            const auto err = parse_stream_error(payload);
            const auto status = err.has_value() ? err->status : k_unauthorized;
            state_->last_error_status = status;
            // A refused handshake looks exactly like an idle account otherwise:
            // the socket stays open and simply never carries a private row.
            state_->sink->on_connectivity(
                {.state = classify(status, {}) == exec::ErrorClass::AuthFailure
                              ? exec::ConnState::AuthFailed
                              : exec::ConnState::Disconnected,
                 .stream_id = k_private_stream_id,
                 .ts = state_->clock->wall()});
            return;
        }
        case TableKind::SubscribeAck:
        case TableKind::Info:
        case TableKind::Quote:
        case TableKind::Trade:
        case TableKind::Instrument:
        case TableKind::OrderBook:
        case TableKind::Unknown:
            // Public tables are never subscribed on this socket. Naming them is
            // what makes a new table a compiler error rather than a dropped one.
            return;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
    Protocol protocol_;
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class BitmexExec final : public exec::ExecutionClient {
public:
    explicit BitmexExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials()) {
            // Every path below signs. Saying so once at start beats one
            // AuthFailure per order for the rest of the session.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "bitmex: credentials not loaded")};
        }
        rest_.start();
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return started_ && client_.connected();
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
        // BitMEX keys a cancel on the order id alone, so unlike most venues here
        // a batch may straddle instruments and only the count needs chunking.
        for (std::size_t at = 0; at < cmds.size(); at += SignedRequestBuilder::k_max_batch_cancel) {
            const auto chunk = cmds.subspan(
                at, std::min(SignedRequestBuilder::k_max_batch_cancel, cmds.size() - at));
            send_batch(chunk);
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        std::vector<exec::OrderStatusReport> reports;
        for (std::uint32_t page = 0; page < k_max_pages; ++page) {
            const auto body = pull(builder_.open_orders(page * SignedRequestBuilder::k_page_size,
                                                        state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto rows = parse_orders(*body, storage);
            if (!rows.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
            }
            for (const auto& wire : *rows) {
                // BitMEX offers no market parameter: one call answers for spot
                // and the perpetuals at once, so the caller's market is applied
                // here rather than pretended at the request.
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
            if (rows->size() < SignedRequestBuilder::k_page_size) {
                break;
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.query_order(id, state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto rows = parse_orders(*body, storage);
        if (!rows.has_value() || rows->empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitmex: no order with that client id")};
        }
        auto report = map_open_order(rows->front(), state_->instruments);
        if (!report.has_value()) {
            // The filter matched, so the order exists; it is simply no longer
            // working. Saying that beats reporting a finished order as live.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "bitmex: order is not in the working set")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        std::vector<exec::FillReport> reports;
        for (std::uint32_t page = 0; page < k_max_pages; ++page) {
            const auto body = pull(builder_.fills(
                since, page * SignedRequestBuilder::k_page_size, state_->clock->wall()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local ExecutionBuffer storage;
            const auto rows = parse_executions(*body, storage);
            if (!rows.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
            }
            for (const auto& wire : *rows) {
                auto report = map_fill(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
            if (rows->size() < SignedRequestBuilder::k_page_size) {
                break;
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.positions(state_->clock->wall()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local PositionBuffer storage;
        const auto rows = parse_positions(*body, storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            auto report = map_position(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // 500 rows a page; four pages is more open orders than any account this
    // framework is aimed at holds, and an unbounded sweep on a cold path is how
    // a reconnect earns a rate-limit ban.
    static constexpr std::uint32_t k_max_pages = 4;

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(std::expected<net::RestRequest, BuildError>&& request) {
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 20'000);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        if (response->status != k_ok) {
            return std::unexpected{rest_failure(*response)};
        }
        return response->body;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->clock->wall();
        auto request = kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
                       : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                              : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind, build_failure(request.error()));
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
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

    void send_batch(std::span<const OrderCmd> chunk) noexcept {
        auto request = builder_.cancel_batch(chunk, state_->instruments, state_->clock->wall());
        if (!request.has_value()) {
            const auto refusal = build_failure(request.error());
            for (const auto& cmd : chunk) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        std::vector<OrderCmd> pending{chunk.begin(), chunk.end()};
        if (!rest_.send(*request, [this, pending](net::RestResponse&& response) {
                on_batch_response(std::move(response), pending);
            })) {
            // Never left this process, so nothing is in doubt.
            for (const auto& cmd : chunk) {
                emit_reject(
                    cmd,
                    Kind::Cancel,
                    exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            }
            return;
        }
        // ON THE WIRE AND UNANSWERED, every member of it: one lost read leaves
        // the whole chunk's outcome unknown, not refused.
        for (const auto& cmd : chunk) {
            (void)unanswered_.add({.client_order_id = cmd.client_order_id,
                                   .exchange_client_order_id = cmd.exchange_client_order_id,
                                   .instrument = cmd.instrument,
                                   .side = cmd.side,
                                   .qty = cmd.qty,
                                   .kind = cmd.kind,
                                   .sent = state_->clock->mono()});
        }
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& pending) {
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            for (const auto& cmd : pending) {
                (void)unanswered_.resolve(cmd.client_order_id);
            }
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The chunk may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            const auto error = exec::VenueError::make(
                exec::ErrorClass::Connectivity,
                0,
                response.error.empty() ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error});
            for (const auto& cmd : pending) {
                state_->sink->on_order_error(cmd.client_order_id, error);
            }
            return;
        }
        if (response.status != k_ok) {
            const auto refusal = rest_failure(response);
            for (const auto& cmd : pending) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        static thread_local OrderBuffer storage;
        const auto rows = parse_orders(response.body, storage);
        if (!rows.has_value()) {
            const auto refusal =
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order rows");
            for (const auto& cmd : pending) {
                emit_reject(cmd, Kind::Cancel, refusal);
            }
            return;
        }
        // The per-order verdict is IN THE ROW: BitMEX answers HTTP 200 and puts
        // `error` on the ones it could not cancel. Reading the status line alone
        // reports every one of them as cancelled.
        for (const auto& wire : *rows) {
            if (wire.error.empty()) {
                publish_order(*state_, wire, FrameAction::Insert, state_->clock->wall());
                continue;
            }
            for (const auto& cmd : pending) {
                if (cmd.target_exchange_order_id.view() == wire.order_id) {
                    emit_reject(cmd,
                                Kind::Cancel,
                                exec::VenueError::make(exec::ErrorClass::Terminal, 0, wire.error));
                }
            }
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        const auto received = state_->clock->wall();
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
        if (response.status != k_ok) {
            // A cancel for an order BitMEX has already finished with is not a
            // failure worth telling the OMS about; reconciliation settles it.
            if (kind == Kind::Cancel && response.status == k_not_found) {
                return;
            }
            emit_reject(cmd, kind, rest_failure(response));
            return;
        }
        if (kind == Kind::Cancel) {
            on_batch_response(std::move(response), std::vector<OrderCmd>{cmd});
            return;
        }
        const auto wire = parse_order(response.body);
        if (!wire.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order row"));
            return;
        }
        if (kind == Kind::Amend) {
            // ordStatus stays "New" through an amend, so the order table can
            // never report one. This response is the only confirmation there is;
            // without it an amended order stays PendingUpdate forever.
            publish_order(*state_, *wire, FrameAction::Update, received);
            emit_updated(*wire, cmd, received);
            return;
        }
        publish_order(*state_, *wire, FrameAction::Insert, received);
    }

    void emit_updated(const WireOrder& wire, const OrderCmd& cmd, UnixNanos received) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Updated;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        if (const auto id = ExchangeOrderId::from(wire.order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = cmd.price;
        event.qty = cmd.qty;
        // PUT /order takes a new TOTAL, and BitMEX answers with the row's own
        // leavesQty; that is what is still working, not the size that was asked.
        event.leaves_qty = cmd.qty;
        if (const auto* memo = state_->recall(std::string{wire.order_id}); memo != nullptr) {
            event.leaves_qty = memo->leaves;
        }
        event.ts_event = received;
        event.ts_init = received;
        state_->sink->on_order_event(event);
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

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient client_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitmexExec>(std::move(state));
}

} // namespace raikiri::venues::bitmex::detail
