#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/coinbase_venue.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::coinbase {

namespace {

// level2_batch's opening snapshot for BTC-USD measured 1,203,894 bytes —
// 45,805 levels in one frame — so the transport's 1 MiB default drops the
// only frame that can start a book. Sized for a book several times that
// rather than for the one measured; the value is a ceiling, not a buffer.
constexpr std::size_t k_max_message_bytes = std::size_t{8} * 1024 * 1024;

// Far above any sane working set; the cap exists so a venue incident cannot
// turn the client-id memo into the heap.
constexpr std::size_t k_max_tracked_orders = 65'536;

// A cancel naming an order Coinbase has already finished.
constexpr int k_http_not_found = 404;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string product_id;
};

// State every coinbase connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    net::Signer signer;
    // Coinbase's third credential. Held as a string because it travels in a
    // header on every signed request and is not itself a signing key.
    std::string passphrase;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_confirmed{0};
    std::uint64_t subscribe_refusals{0};
    exec::VenueError last_error{};

    // The UUID this adapter derived from a ULID cannot be turned back into
    // one, so the mapping is remembered for as long as the process lives. An
    // order placed by an EARLIER process comes back from a sweep with no
    // client id, which is a gap reconciliation must be told about rather than
    // one that is quietly filled with a wrong id.
    std::unordered_map<std::string, ExchangeClientOrderId> client_ids;

    // Deribit-style venue clock correction: Coinbase rejects a signature more
    // than 30 seconds from its own clock.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = static_cast<std::int64_t>(clock->wall().ns);
        return UnixNanos{static_cast<std::uint64_t>(local + clock_offset_ns)};
    }

    void remember(const ExchangeClientOrderId& id) {
        const auto uuid = client_oid_of(id);
        if (!uuid.has_value() || client_ids.size() >= k_max_tracked_orders) {
            return;
        }
        client_ids.emplace(std::string{uuid->data(), uuid->size()}, id);
    }

    [[nodiscard]] ExchangeClientOrderId recall(std::string_view client_oid) const {
        const auto at = client_ids.find(std::string{client_oid});
        return at == client_ids.end() ? ExchangeClientOrderId{} : at->second;
    }

    void add_subscription(std::string_view channel, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.product_id == product_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{product_id});
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

// The book's own ts_event: every Coinbase timestamp is ISO-8601 text, and an
// unreadable one falls back to ingest rather than to zero.
UnixNanos book_time(std::string_view text, UnixNanos fallback) {
    const auto parsed = parse_iso8601(text);
    return parsed.has_value() ? *parsed : fallback;
}

void publish(VenueState& state, Book& book, std::string_view time, UnixNanos received) {
    if (const auto depth = book.snapshot(book_time(time, received), received); depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Match: {
        const auto wire = parse_match(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::Snapshot: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_l2_snapshot(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->product_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        if (book.reset(*wire) != BookResult::Resynced) {
            return;
        }
        publish(state, book, wire->time, received);
        break;
    }
    case EventKind::L2Update: {
        static thread_local ChangeBuffer changes;
        const auto wire = parse_l2_update(frame, changes);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->product_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        if (book.apply(*wire) != BookResult::Applied) {
            return; // unsynced or dropped: silence, never a guess
        }
        publish(state, book, wire->time, received);
        break;
    }
    case EventKind::Subscriptions: {
        static thread_local AckBuffer ack;
        if (const auto channels = parse_subscriptions(frame, ack); channels.has_value()) {
            state.channels_confirmed = channels->size();
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            ++state.subscribe_refusals;
            state.last_error =
                exec::VenueError::make(classify_stream(err->message), 0, err->reason);
        }
        break;
    }
    case EventKind::Unknown:
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
            views.push_back({.channel = entry.channel, .product_id = entry.product_id});
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
        // A fresh snapshot is coming and it replaces the book wholesale; what
        // is held now describes a subscription that no longer exists.
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

class CoinbaseData final : public exec::DataClient {
public:
    explicit CoinbaseData(std::shared_ptr<VenueState> state)
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
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(channel, symbol);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{
                Subscription{.channel = channel, .product_id = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        (void)now;
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
                                                std::uint64_t timeout_ms = 10'000) {
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

void sync_clock(VenueState& state, net::RestClient& rest) {
    const auto sent = state.clock->wall();
    const auto response = await_response(state, rest, state.builder.server_time(), 5'000);
    if (!response.has_value() || response->transport_failed()) {
        return;
    }
    const auto iso = parse_server_time(response->body);
    if (!iso.has_value()) {
        return;
    }
    const auto venue = parse_iso8601(*iso);
    if (!venue.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(venue->ns) - static_cast<std::int64_t>(midpoint);
}

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. NotSigned is the exception, and it points at the
// config rather than at the venue.
// The body may not parse; the status classifies either way.
exec::VenueError http_failure(const net::RestResponse& response) noexcept {
    const auto message = parse_rest_error(response.body);
    return exec::VenueError::make(classify_http(response.status),
                                  response.status,
                                  message.has_value() ? *message
                                                      : std::string_view{"http error"});
}

exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// Live order entry over REST.
//
// There is no private socket here, and the omission is named rather than
// implied: Coinbase Exchange's real-time private feed is the per-product
// `user` channel, and an ExecutionClient is never told which products to
// watch. What that costs is EXECUTIONS — every other lifecycle step is
// answered synchronously, because POST /orders returns the whole order object
// and DELETE /orders answers only when the cancel took. fill_reports says the
// same thing in the same words rather than answering with an empty vector.
class CoinbaseExec final : public exec::ExecutionClient {
public:
    explicit CoinbaseExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}},
          builder_{state_->signer, state_->passphrase} {}

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials() || state_->passphrase.empty()) {
            // AuthFailure, not Terminal: the first sends an operator to the
            // config, the second to a venue status page. The passphrase is a
            // third credential and the one most often left unset.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "coinbase: key, secret and passphrase required")};
        }
        rest_.start();
        sync_clock(*state_, rest_);
        started_ = true;
        return {};
    }

    void stop() override {
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return started_; }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
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
        const auto orders = parse_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_open_order(wire, state_->instruments,
                                             state_->recall(wire.client_oid));
                report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        const auto request = builder_.query_order(id, market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failure(request.error())};
        }
        const auto body = fetch(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto order = parse_order(*body);
        if (!order.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(order.error()))};
        }
        // The id asked about is the one reported back: the UUID that travelled
        // cannot be turned into a ULID, and the caller already holds the one
        // it wanted.
        auto report = map_open_order(*order, state_->instruments, id);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not mappable")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        (void)since;
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // GET /fills requires order_id OR product_id — "Either `order_id` or
        // `product_id` is required" — and this call names only a market.
        // Sweeping the 800-odd listed products would be a guess at which ones
        // matter, and an empty vector reads as "no fills", the one answer a
        // reconciler must not be handed wrongly.
        return std::unexpected{exec::not_implemented("coinbase fill pull is per-product")};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // A coin balance is not a position: one BTC balance backs BTC-USD,
        // BTC-EUR and BTC-USDT alike, and there is no InstrumentId to report
        // it against. Answering "flat" would be a claim about a concept this
        // API does not have.
        return std::unexpected{exec::not_implemented("coinbase spot position pull")};
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
            const auto message = parse_rest_error(response->body);
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status,
                message.has_value() ? *message : std::string_view{"http error"})};
        }
        return response->body;
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
        if (kind == Kind::Create) {
            // Remembered BEFORE the send: the response can arrive inside the
            // same poll, and a sweep that raced it would find no id to map.
            state_->remember(cmd.exchange_client_order_id);
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(
                cmd, kind, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) {
        if (response.transport_failed()) {
            emit_reject(cmd,
                        kind, // no verdict from the venue at all
                        exec::VenueError::make(
                            exec::ErrorClass::Connectivity, 0, "no response from venue"));
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            // A cancel naming an order Coinbase no longer has answers 404 with
            // {"message":"order not found"}. The order finished between the
            // decision and the request; escalating would raise a
            // CancelRejected against an order the FSM has already closed.
            if (kind == Kind::Cancel && response.status == k_http_not_found) {
                return;
            }
            emit_reject(cmd, kind, http_failure(response));
            return;
        }
        switch (kind) {
        case Kind::Create: {
            // POST /orders answers with the whole order object, so acceptance
            // and a same-instant rejection are both stated here rather than
            // waiting on a stream this adapter does not hold.
            const auto order = parse_order(response.body);
            if (!order.has_value()) {
                emit_reject(
                    cmd,
                    kind,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable order row"));
                return;
            }
            if (const auto event = map_order_event(*order, state_->instruments,
                                                   cmd.exchange_client_order_id,
                                                   state_->clock->wall());
                event.has_value()) {
                state_->sink->on_order_event(*event);
            }
            return;
        }
        case Kind::Cancel:
            // The body is the cancelled order's id and nothing else, so the
            // status line is the whole verdict.
            emit_cancelled(cmd);
            return;
        case Kind::Amend:
            return; // unreachable: amend_order never builds a request
        }
    }

    void emit_cancelled(const OrderCmd& cmd) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::Canceled;
        event.side = cmd.side;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.exchange_order_id = cmd.target_exchange_order_id;
        event.qty = cmd.qty;
        event.price = cmd.price;
        // Nothing rests on the book after a cancel, and that is what this
        // field means. How much of the order had already filled is a separate
        // question the cancel reply does not answer and the Filled events do.
        event.leaves_qty = QtyLots::from_raw(0);
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
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
        // A create that never reached the book leaves nothing behind; a
        // refused cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    SignedRequestBuilder builder_;
    bool started_{false};
};

class CoinbaseInstruments final : public exec::InstrumentSource {
public:
    explicit CoinbaseInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        if (market != MarketType::Spot) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "coinbase exchange lists spot only")};
        }
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.products(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (response->status != 200) {
            const auto message = parse_rest_error(response->body);
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status),
                0,
                message.has_value() ? *message : std::string_view{"catalogue request refused"})};
        }

        static thread_local ProductBuffer storage;
        const auto wire = parse_products(response->body, storage);
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
                continue; // not online, disabled, or missing an increment
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
    auto state = std::make_shared<VenueState>(sink, clock);

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
            config::Secret encoded;
            if (provider.resolve(*ref, encoded).has_value()) {
                // Decoded ONCE, here. Coinbase keys the HMAC with the raw
                // bytes behind the printable secret, and a signer holding the
                // printable form produces a well-formed digest the venue
                // answers 401 to — indistinguishable from a revoked key.
                std::array<char, k_max_secret_bytes> raw{};
                if (const auto len = decode_secret(encoded.view(), raw); len.has_value()) {
                    (void)state->signer.secret().assign(std::string_view{raw.data(), *len});
                }
            }
        }
    }
    if (!config.passphrase_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.passphrase_env); ref.has_value()) {
            config::Secret phrase;
            if (provider.resolve(*ref, phrase).has_value()) {
                state->passphrase = std::string{phrase.view()};
            }
        }
    }

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Coinbase);
    adapter.data = std::make_unique<CoinbaseData>(state);
    adapter.execution = std::make_unique<CoinbaseExec>(state);
    adapter.instruments = std::make_unique<CoinbaseInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Coinbase, &make);
}

} // namespace raikiri::venues::coinbase
