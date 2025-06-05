#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/errors.hpp>
#include <raikiri/venues/kraken/kraken_venue.hpp>
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

namespace raikiri::venues::kraken {

namespace {

// Kraken pushes the whole book, not a fixed depth: a live PF_XBTUSD snapshot
// measured 107 KiB, so the transport's 1 MiB default gets headroom.
constexpr std::size_t k_max_message_bytes = std::size_t{4} * 1024 * 1024;

// The private socket is its own connection, so it reports its own health.
constexpr std::uint32_t k_private_stream_id = 1;

// A ceiling on what one process remembers about working orders. The memo is
// the ONLY thing that names an instrument when a removal delta arrives: that
// frame carries an order id and nothing else.
constexpr std::size_t k_max_live_orders = 8'192;

// /fills answers at most 100 rows and pages BACKWARDS from `lastFillTime`, so
// a `since` older than this many pages is answered with what we could reach
// rather than with an unbounded sweep of the account's history.
constexpr std::size_t k_max_fill_pages = 8;

// What /fills answers in one call; a shorter page is the end of the history.
constexpr std::size_t k_fills_per_page = 100;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string feed;
    std::string product_id;
};

// What an order removal frame does not say. `open_orders` reports a departure
// as {"order_id":..., "is_cancel":true, "reason":...} — no instrument, no
// side, no size — so an adapter that did not write this down cannot emit the
// event at all.
struct LiveOrder {
    InstrumentId instrument{};
    ExchangeClientOrderId client_order_id{};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    OrderFlags flags{OrderFlags::None};
    PriceTicks price{};
    QtyLots qty{};
    QtyLots leaves{};
};

// State every kraken connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;
    std::unordered_map<std::string, LiveOrder> live_orders;

    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void remember(std::string_view order_id, const LiveOrder& order) {
        if (order_id.empty()) {
            return;
        }
        const std::string key{order_id};
        if (live_orders.size() >= k_max_live_orders && !live_orders.contains(key)) {
            return; // a bounded memo beats an unbounded one that OOMs the gateway
        }
        live_orders[key] = order;
    }

    [[nodiscard]] const LiveOrder* recall(std::string_view order_id) const {
        const auto it = live_orders.find(std::string{order_id});
        return it == live_orders.end() ? nullptr : &it->second;
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }

    void add_subscription(std::string_view feed, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.feed == feed && existing.product_id == product_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        const auto after_feed = std::ranges::find_if(
            desired, [&](const OwnedSubscription& entry) { return entry.feed != feed; });
        desired.insert(
            after_feed,
            OwnedSubscription{.feed = std::string{feed}, .product_id = std::string{product_id}});
    }
};

void handle_book(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->product_id);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(*wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced, redelivered or dropped: silence, never a guess
    }
    if (const auto depth = book.snapshot(ms_to_unix(wire->timestamp_ms, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Ticker:
    case EventKind::TickerLite: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Book:
        handle_book(state, frame, received);
        break;
    case EventKind::Alert:
        if (parse_stream_alert(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    case EventKind::Subscribed:
    case EventKind::Info:
    case EventKind::Unknown:
        break;
    // Private frames ride the same socket but are routed by the private
    // handler, which owns the signed challenge. Naming them here rather than
    // letting a default swallow them is what makes a new private feed a
    // compiler error instead of a silently dropped frame.
    case EventKind::Challenge:
    case EventKind::OpenOrdersSnapshot:
    case EventKind::OpenOrders:
    case EventKind::Fills:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.feed = entry.feed, .product_id = entry.product_id});
        }
        std::size_t sent = 0;
        while (sent < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
            sent += frame->consumed;
        }
        // Whatever the seq chain said before the socket dropped says nothing
        // about the book_snapshot Kraken is about to send.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class KrakenData final : public exec::DataClient {
public:
    explicit KrakenData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()},
                                .max_message_bytes = k_max_message_bytes},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (state_->testnet) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kraken futures has no demo environment")};
        }
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto feed = feed_for(kind);
        if (feed.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        const auto before = state_->desired.size();
        state_->add_subscription(feed, symbol);
        if (state_->desired.size() == before) {
            return {};
        }
        if (client_.connected()) {
            const std::array<Subscription, 1> one{Subscription{.feed = feed, .product_id = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms = 20'000) {
    std::optional<net::RestResponse> result;
    if (!rest.send(request,
                   [&result](net::RestResponse&& response) { result = std::move(response); })) {
        return std::nullopt;
    }
    const auto deadline = state.clock->mono().ns + timeout_ms * 1'000'000ULL;
    while (!result.has_value() && state.clock->mono().ns < deadline) {
        if (state.io.poll() == 0) {
            state.io.run_for(5);
        }
    }
    return result;
}

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
        const auto signature =
            authent(state_->signer.secret().view(),
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
            if (const auto event = map_ws_order(wire,
                                                OrderEventType::Accepted,
                                                state_->instruments,
                                                received);
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
            const auto event = map_ws_order(delta->order,
                                            event_type_of(effect),
                                            state_->instruments,
                                            received);
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
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
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
                if (auto report = map_rest_fill(wire, state_->instruments);
                    report.has_value()) {
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
        auto request = kind == Kind::Create ? builder_.create_order(cmd, state_->instruments, now)
                       : kind == Kind::Cancel
                           ? builder_.cancel_order(cmd, state_->instruments, now)
                           : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(cmd, kind);
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(cmd, kind);
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        if (response.transport_failed()) {
            emit_reject(cmd, kind); // no verdict from the venue at all
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (response.status < 200 || response.status >= 300 || !envelope.has_value() ||
            !envelope->ok) {
            if (kind == Kind::Cancel && envelope.has_value() &&
                is_benign_cancel_failure(envelope->error)) {
                return; // the order already finished; reconciliation settles it
            }
            emit_reject(cmd, kind);
            return;
        }
        // The envelope says "success" for a refused order as readily as for an
        // accepted one; `status` is the verdict.
        const auto verdict = parse_order_status(response.body, status_kind(kind));
        if (!verdict.has_value()) {
            emit_reject(cmd, kind);
            return;
        }
        switch (kind) {
        case Kind::Create:
            if (!is_send_accepted(verdict->status)) {
                emit_reject(cmd, kind);
                return;
            }
            // Accepted is published by the open_orders delta, which carries the
            // venue's own view of the resting order. Binding the id here is
            // what lets a fill that overtakes that delta still route.
            bind_ack(verdict->order_id, cmd);
            return;
        case Kind::Cancel:
            if (!is_cancel_accepted(verdict->status)) {
                emit_reject(cmd, kind);
            }
            return;
        case Kind::Amend:
            if (!is_edit_accepted(verdict->status)) {
                emit_reject(cmd, kind);
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

    void emit_reject(const OrderCmd& cmd, Kind kind) noexcept {
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
};

class KrakenInstruments final : public exec::InstrumentSource {
public:
    explicit KrakenInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{rest_host()}, .port = "443", .timeout_ms = 30'000}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Perp) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kraken adapter serves futures only")};
        }
        rest_.start();

        const auto sent = state_->clock->wall();
        const auto response = await_response(*state_, rest_, state_->builder.instruments());
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        const auto envelope = parse_rest_envelope(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (!envelope->ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->error), 0, envelope->error)};
        }
        // Half the round trip is the usual estimate of one-way delay.
        if (envelope->server_ms != 0) {
            const auto received = state_->clock->wall();
            const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
            state_->clock_offset_ns =
                static_cast<std::int64_t>(envelope->server_ms * 1'000'000ULL) -
                static_cast<std::int64_t>(midpoint);
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_instruments(response->body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, market);
            if (!instrument.has_value()) {
                continue; // not tradeable, dated, or missing a step
            }
            const auto id = state_->instruments.add(*instrument);
            if (!id.has_value()) {
                continue; // duplicate or table full: reported by count, not by throw
            }
            loaded.push_back(*state_->instruments.find(*id));
        }
        return loaded;
    }

private:
    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state->signer.set_api_key(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            // Stored in its PRINTABLE base64 form: sign.hpp decodes it at every
            // signature, because the decoded bytes are the HMAC key.
            (void)provider.resolve(*ref, state->signer.secret());
        }
    }
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Kraken);
    adapter.data = std::make_unique<KrakenData>(state);
    adapter.execution = std::make_unique<KrakenExec>(state);
    adapter.instruments = std::make_unique<KrakenInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Kraken, &make);
}

} // namespace raikiri::venues::kraken
