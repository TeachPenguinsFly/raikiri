#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/venues/kraken/rest.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kraken::detail {

namespace {

// The private socket is its own connection, so it reports its own health.
constexpr std::uint32_t k_private_stream_id = 1;

// /fills answers at most 100 rows and pages BACKWARDS from `lastFillTime`, so
// a `since` older than this many pages is answered with what we could reach
// rather than with an unbounded sweep of the account's history.
constexpr std::size_t k_max_fill_pages = 8;

// What /fills answers in one call; a shorter page is the end of the history.
constexpr std::size_t k_fills_per_page = 100;

// Every market this adapter can act on, named exhaustively so Option is a case
// the compiler forces a decision about rather than something a default arm
// swallows. Named BEFORE the instrument table is read: InstrumentTable::find
// compares the whole id, so an id for a market we never loaded resolves to
// nothing and would otherwise be reported as an unknown instrument.
exec::VenueError refuse_market_error(BuildError err) noexcept {
    switch (err) {
    case BuildError::SpotUnsupported:
        // Kraken Spot is api.kraken.com: another host, another symbology and
        // another signature. This adapter never loaded it and cannot sign for
        // it, so it says so instead of blaming a stale catalogue.
        return exec::not_implemented("kraken spot order entry");
    case BuildError::DatedFuturesUnsupported:
        return exec::not_implemented("kraken dated futures order entry");
    case BuildError::OptionsUnsupported:
        return exec::not_implemented("kraken options order entry");
    case BuildError::UnknownInstrument:
    case BuildError::BadQuantity:
    case BuildError::BadPrice:
    case BuildError::BufferTooSmall:
    case BuildError::NotSigned:
    case BuildError::NoOrderTarget:
        break;
    }
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    const auto refusal = market_refusal(market);
    return refusal.has_value() ? std::optional{refuse_market_error(*refusal)} : std::nullopt;
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. Missing credentials are the exception the
// reconciler acts on differently, which is why they keep their own class.
exec::VenueError build_failure(BuildError err) noexcept {
    if (err == BuildError::NotSigned) {
        return exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, to_string(err));
    }
    return refuse_market_error(err);
}

exec::VenueError rest_failure(std::string_view body) {
    const auto envelope = parse_rest_envelope(body);
    if (!envelope.has_value()) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable refusal body");
    }
    if (envelope->ok) {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "refusal body reports ok");
    }
    return exec::VenueError::make(classify(envelope->error), 0, envelope->error);
}

OrderEventType event_type_of(DeltaEffect effect) noexcept {
    switch (effect) {
    case DeltaEffect::Triggered:
        return OrderEventType::Triggered;
    case DeltaEffect::Canceled:
        return OrderEventType::Canceled;
    case DeltaEffect::Rejected:
        return OrderEventType::Rejected;
    case DeltaEffect::Accepted:
    case DeltaEffect::Silent:
        break;
    }
    // Silent frames still update the memo, and the type they carry is never
    // published; Accepted is the honest placeholder for one.
    return OrderEventType::Accepted;
}

LiveOrder memo_of(const OrderEvent& event) noexcept {
    return {.instrument = event.instrument,
            .client_order_id = event.exchange_client_order_id,
            .side = event.side,
            .order_type = event.order_type,
            .flags = event.flags,
            .price = event.price,
            .qty = event.qty,
            .leaves = event.leaves_qty};
}

// Kraken's private feeds ride the same socket shape as the public ones, but the
// subscribe carries a signed challenge that is valid for ONE connection. That
// is why this is a second WsClient rather than another subscription on the
// market-data one: a public reconnect must not have to re-run the handshake,
// and an order stream must not drop because a book resubscribe failed.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    void on_open() override {
        // The challenge is per CONNECTION: a signature minted on the previous
        // socket authenticates nothing on this one.
        challenge_len_ = 0;
        signed_len_ = 0;
        const auto frame = protocol_.challenge_request_frame(state_->signer.api_key());
        if (!frame.has_value() || !client_->send(frame->text)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Challenge:
            on_challenge(payload);
            return;
        case EventKind::OpenOrdersSnapshot:
            on_snapshot(payload, received);
            return;
        case EventKind::OpenOrders:
            on_delta(payload, received);
            return;
        case EventKind::Fills:
            on_fills(payload, received);
            return;
        case EventKind::Alert:
            // A refused private subscribe is an authentication failure wearing
            // the same {"event":"alert"} shape a bad product id wears; without
            // this the stream is simply silent.
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        case EventKind::Subscribed:
        case EventKind::Info:
        case EventKind::Ticker:
        case EventKind::TickerLite:
        case EventKind::Trade:
        case EventKind::Book:
        case EventKind::Unknown:
            // Public feeds are never subscribed on this socket. Naming them is
            // what makes a new feed a compiler error rather than a dropped one.
            return;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

private:
    void on_challenge(std::string_view frame) noexcept {
        const auto wire = parse_challenge(frame);
        if (!wire.has_value() || wire->message.empty() ||
            wire->message.size() > challenge_.size()) {
            return;
        }
        std::memcpy(challenge_.data(), wire->message.data(), wire->message.size());
        challenge_len_ = wire->message.size();
        const auto signature = authent(state_->signer.secret().view(),
                                       std::string_view{challenge_.data(), challenge_len_},
                                       signed_challenge_);
        if (signature.empty()) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        signed_len_ = signature.size();
        if (!subscribe(k_feed_open_orders) || !subscribe(k_feed_fills)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_private_stream_id,
                                           .ts = state_->clock->wall()});
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    [[nodiscard]] bool subscribe(std::string_view feed) noexcept {
        const auto frame =
            protocol_.private_subscribe_frame(feed,
                                              state_->signer.api_key(),
                                              {challenge_.data(), challenge_len_},
                                              {signed_challenge_.data(), signed_len_});
        return frame.has_value() && client_->send(frame->text);
    }

    // The snapshot describes the working set at connect time. It is remembered
    // and NOT published: an order the OMS already knows about would arrive as a
    // second Accepted, and reconciliation — not a replay — is what settles a
    // gap.
    void on_snapshot(std::string_view frame, UnixNanos received) noexcept {
        static thread_local OrderSnapshotBuffer storage;
        const auto orders = parse_order_snapshot(frame, storage);
        if (!orders.has_value()) {
            return;
        }
        for (const auto& wire : *orders) {
            if (const auto event =
                    map_ws_order(wire, OrderEventType::Accepted, state_->instruments, received);
                event.has_value()) {
                state_->remember(wire.order_id, memo_of(*event));
            }
        }
    }

    void on_delta(std::string_view frame, UnixNanos received) noexcept {
        const auto delta = parse_order_delta(frame);
        if (!delta.has_value()) {
            return;
        }
        const auto effect = effect_of(delta->reason, delta->is_cancel);
        if (delta->has_order) {
            const auto event =
                map_ws_order(delta->order, event_type_of(effect), state_->instruments, received);
            if (!event.has_value()) {
                return;
            }
            state_->remember(delta->order.order_id, memo_of(*event));
            if (effect != DeltaEffect::Silent) {
                state_->sink->on_order_event(*event);
            }
            return;
        }
        // A removal names an order id and nothing else. Without the memo there
        // is no instrument to attribute it to, and an event with the wrong
        // instrument is worse than one that never fires.
        const auto* memo = state_->recall(delta->order_id);
        if (memo == nullptr) {
            return;
        }
        if (effect != DeltaEffect::Silent) {
            OrderEvent event{};
            event.type = event_type_of(effect);
            event.instrument = memo->instrument;
            event.side = memo->side;
            event.order_type = memo->order_type;
            event.flags = memo->flags;
            event.exchange_client_order_id = memo->client_order_id;
            if (const auto id = ExchangeOrderId::from(delta->order_id); id.has_value()) {
                event.exchange_order_id = *id;
            }
            event.price = memo->price;
            event.qty = memo->qty;
            event.leaves_qty = QtyLots::from_raw(0);
            event.ts_event = received;
            event.ts_init = received;
            state_->sink->on_order_event(event);
        }
        state_->live_orders.erase(std::string{delta->order_id});
    }

    void on_fills(std::string_view frame, UnixNanos received) noexcept {
        static thread_local WsFillBuffer storage;
        const auto fills = parse_ws_fills(frame, storage);
        if (!fills.has_value()) {
            return;
        }
        for (const auto& wire : *fills) {
            const auto event = map_ws_fill(wire, state_->instruments, received);
            if (!event.has_value()) {
                continue;
            }
            if (auto* memo = mutable_memo(wire.order_id); memo != nullptr) {
                memo->leaves = event->leaves_qty;
            }
            state_->sink->on_order_event(*event);
        }
    }

    [[nodiscard]] LiveOrder* mutable_memo(std::string_view order_id) {
        const auto it = state_->live_orders.find(std::string{order_id});
        return it == state_->live_orders.end() ? nullptr : &it->second;
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    Protocol protocol_;
    // A UUID is 36 characters; the slack is for a challenge Kraken lengthens.
    std::array<char, 64> challenge_{};
    std::array<char, k_authent_bytes> signed_challenge_{};
    std::size_t challenge_len_{0};
    std::size_t signed_len_{0};
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class KrakenExec final : public exec::ExecutionClient {
public:
    explicit KrakenExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()},
                                .max_message_bytes = k_max_message_bytes},
                  handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kraken futures has no demo environment")};
        }
        if (!state_->signer.has_credentials()) {
            // Every path below signs. Saying so once at start beats one
            // AuthFailure per order for the rest of the session.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kraken: credentials not loaded")};
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
            // UNDEF, not zero: zero leaves would read as "nothing is working".
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
        const auto body = pull(builder_.open_orders(state_->venue_now()));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OpenOrderBuffer storage;
        const auto orders = parse_open_orders(*body, storage);
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
        // There is no single-order read in this API: /openorders is the whole
        // working set, filtered here. An order that has finished is therefore
        // not findable, and saying so beats reporting it as unknown.
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id == id) {
                return report;
            }
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "kraken: order is not in the working set")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto since_ms = since.ns / 1'000'000ULL;
        std::vector<exec::FillReport> reports;
        std::string cursor; // empty asks for the newest page
        for (std::size_t page = 0; page < k_max_fill_pages; ++page) {
            const auto body = pull(builder_.fills(cursor, state_->venue_now()));
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local FillBuffer storage;
            const auto fills = parse_fills(*body, storage);
            if (!fills.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(fills.error()))};
            }
            if (fills->empty()) {
                break;
            }
            // Rows come back newest first, so the last one carries the cursor
            // for the page before this one.
            std::uint64_t oldest_ms = 0;
            std::string_view oldest_text;
            bool reached_since = false;
            for (const auto& wire : *fills) {
                if (oldest_ms == 0 || (wire.fill_ms != 0 && wire.fill_ms < oldest_ms)) {
                    oldest_ms = wire.fill_ms;
                    oldest_text = wire.fill_time;
                }
                if (wire.fill_ms < since_ms) {
                    reached_since = true;
                    continue;
                }
                if (auto report = map_rest_fill(wire, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
            // A short page is the beginning of the account's history: the
            // endpoint answers "the 100 entries before lastFillTime", so fewer
            // than that means there is nothing older to ask for.
            if (reached_since || oldest_text.empty() || fills->size() < k_fills_per_page) {
                break;
            }
            cursor.assign(oldest_text);
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        const auto body = pull(builder_.positions(state_->venue_now()));
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

    static StatusKind status_kind(Kind kind) noexcept {
        switch (kind) {
        case Kind::Cancel:
            return StatusKind::Cancel;
        case Kind::Amend:
            return StatusKind::Edit;
        case Kind::Create:
            break;
        }
        return StatusKind::Send;
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    pull(std::expected<net::RestRequest, BuildError>&& request) {
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        // HTTP status is no substitute for the body: an unauthenticated read
        // answers HTTP 200 with "authenticationError".
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value() || !envelope->ok) {
            return std::unexpected{rest_failure(response->body)};
        }
        return response->body;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
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
            emit_reject(cmd,
                        kind,
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

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
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
        const auto envelope = parse_rest_envelope(response.body);
        if (response.status < 200 || response.status >= 300 || !envelope.has_value() ||
            !envelope->ok) {
            if (kind == Kind::Cancel && envelope.has_value() &&
                is_benign_cancel_failure(envelope->error)) {
                return; // the order already finished; reconciliation settles it
            }
            emit_reject(cmd, kind, rest_failure(response.body));
            return;
        }
        // The envelope says "success" for a refused order as readily as for an
        // accepted one; `status` is the verdict.
        const auto verdict = parse_order_status(response.body, status_kind(kind));
        if (!verdict.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "no status in the response"));
            return;
        }
        switch (kind) {
        case Kind::Create:
            if (!is_send_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // Accepted is published by the open_orders delta, which carries the
            // venue's own view of the resting order. Binding the id here is
            // what lets a fill that overtakes that delta still route.
            bind_ack(verdict->order_id, cmd);
            return;
        case Kind::Cancel:
            if (!is_cancel_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
            }
            return;
        case Kind::Amend:
            if (!is_edit_accepted(verdict->status)) {
                emit_reject(cmd,
                            kind,
                            exec::VenueError::make(exec::ErrorClass::Terminal, 0, verdict->status));
                return;
            }
            // The open_orders feed has no documented `reason` for an edit, so
            // this response is the only confirmation there is; without it an
            // amended order stays PendingUpdate forever.
            emit_updated(verdict->order_id, cmd);
            return;
        }
    }

    void bind_ack(std::string_view order_id, const OrderCmd& cmd) {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            return;
        }
        state_->remember(order_id,
                         LiveOrder{.instrument = cmd.instrument,
                                   .client_order_id = cmd.exchange_client_order_id,
                                   .side = cmd.side,
                                   .order_type = cmd.order_type,
                                   .flags = cmd.flags,
                                   .price = cmd.price,
                                   .qty = cmd.qty,
                                   .leaves = cmd.qty});
    }

    void emit_updated(std::string_view order_id, const OrderCmd& cmd) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Updated;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        if (const auto id = ExchangeOrderId::from(order_id); id.has_value()) {
            event.exchange_order_id = *id;
        }
        event.price = cmd.price;
        event.qty = cmd.qty;
        // editorder RESIZES rather than tops up: `size` is the new total, so
        // what is left of it is the whole of it minus what already traded, and
        // the fills feed is what walks it down from here.
        event.leaves_qty = cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
        if (const auto* memo = state_->recall(order_id); memo != nullptr) {
            auto updated = *memo;
            updated.price = cmd.price;
            updated.qty = cmd.qty;
            updated.leaves = cmd.qty;
            state_->remember(order_id, updated);
        }
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
    return std::make_unique<KrakenExec>(std::move(state));
}

} // namespace raikiri::venues::kraken::detail
