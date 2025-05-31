#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/okx_venue.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::okx {

namespace {

// OKX drops a connection with no application message for 30 s and does not
// count websocket control frames toward that timer, so the app ping runs well
// inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
};

// State every okx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    net::Signer signer;
    config::Secret passphrase;
    exec::InstrumentTable instruments;
    Protocol protocol;
    // The private socket builds its frames from the same class; giving it its
    // own instance keeps one socket's scratch buffer out of the other's.
    Protocol private_protocol;
    RequestBuilder builder;

    // Which markets this process was configured for. The private order
    // channel subscribes by instrument type, so it needs to be told.
    bool spot_enabled{false};
    bool perp_enabled{true};

    std::vector<OwnedSubscription> desired;

    std::unordered_map<InstrumentId, Book> books;
    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset, measured against /api/v5/public/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    [[nodiscard]] bool credentialed() const noexcept {
        return signer.has_credentials() && !passphrase.empty();
    }

    // OKX refuses a signature more than 30 s from ITS clock (50102), so a
    // laptop drifting a minute looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{inst_id});
    }

    Book& book_for(const Instrument& instrument) {
        const auto& base = base_of(instrument);
        const auto it = books.find(base.id);
        if (it != books.end()) {
            return it->second;
        }
        return books.emplace(base.id, Book{base.id, base.tick_size, base.lot_size}).first->second;
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (event->kind) {
    case EventKind::Bbo: {
        const auto wire = parse_bbo(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
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
    case EventKind::FundingRate: {
        const auto wire = parse_funding_rate(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->inst_id);
        if (instrument == nullptr) {
            return;
        }
        auto& book = state.book_for(*instrument);
        const auto result = book.apply(*wire);
        if (result == BookResult::GapDetected || result == BookResult::BadLevel) {
            return; // the venue resends a snapshot; until then we publish nothing
        }
        if (const auto depth = book.snapshot(ms_to_unix(wire->ts_ms, received), received);
            depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::MarkPrice:
    case EventKind::SubscribeAck:
    case EventKind::LoginAck:
    case EventKind::Orders:
    case EventKind::Unknown:
        break; // control frames, and the private topics of the other socket
    }
}

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
        if (const auto fill = map_order_fill(wire, state.instruments, received);
            fill.has_value()) {
            state.sink->on_order_event(*fill);
        }
        if (const auto event = map_order_event(wire, state.instruments, received);
            event.has_value()) {
            state.sink->on_order_event(*event);
        }
    }
}

// Defined below, beside the catalogue loader they share a transport with.
std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms);
void sync_clock(VenueState& state, net::RestClient& rest);

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.channel = entry.channel, .inst_id = entry.inst_id});
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
        // Whatever the seqId chain said before the socket dropped says nothing
        // about the snapshot OKX is about to send.
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

class OkxData final : public exec::DataClient {
public:
    explicit OkxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
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
                Subscription{.channel = channel, .inst_id = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

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
            emit(cmd, OrderEventType::CancelRejected);
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
            if (const auto report = map_open_order(wire, state_->instruments);
                report.has_value()) {
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
            return std::unexpected{
                exec::VenueError::make(request.error() == BuildError::NotSigned
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
            emit(cmd, on_failure);
            return;
        }
        const auto captured = cmd;
        const auto sent = rest_.send(*request, [this, captured, on_failure](net::RestResponse&& r) {
            if (verdict(r, captured.exchange_client_order_id, on_failure) == Verdict::Refused) {
                emit(captured, on_failure);
            }
        });
        if (!sent) {
            emit(cmd, on_failure);
        }
    }

    bool send_batch(const net::RestRequest& request, std::span<const OrderCmd> chunk) noexcept {
        std::vector<OrderCmd> captured{chunk.begin(), chunk.end()};
        return rest_.send(request, [this, captured](net::RestResponse&& r) {
            for (const auto& cmd : captured) {
                if (verdict(r, cmd.exchange_client_order_id, OrderEventType::CancelRejected) ==
                    Verdict::Refused) {
                    emit(cmd, OrderEventType::CancelRejected);
                }
            }
        });
    }

    enum class Verdict : std::uint8_t { Accepted, Refused };

    // OKX answers a refused single order with envelope code "1" — "Operation
    // failed" — and puts the reason in data[].sCode. Reading only the
    // envelope turns every rejection into the same unclassifiable failure,
    // and reading only sCode misses a request that never reached the matcher.
    [[nodiscard]] Verdict verdict(const net::RestResponse& response,
                                  const ExchangeClientOrderId& id,
                                  OrderEventType on_failure) const {
        if (response.transport_failed()) {
            return Verdict::Refused; // no verdict from the venue at all
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            return Verdict::Refused;
        }
        static thread_local AckBuffer storage;
        const auto acks = parse_order_acks(response.body, storage);
        std::int64_t code = envelope->code;
        if (acks.has_value()) {
            for (const auto& ack : *acks) {
                // A batch answers for several orders at once, so the entry
                // that matters is the one carrying this order's id.
                if (ack.cl_ord_id == id.view() || acks->size() == 1) {
                    code = ack.s_code;
                    break;
                }
            }
        }
        if (code == k_ok) {
            return Verdict::Accepted; // the private stream reports what follows
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(code)) {
            return Verdict::Accepted; // the order already filled or was gone
        }
        if (on_failure == OrderEventType::ModifyRejected && is_benign_amend_failure(code)) {
            return Verdict::Accepted;
        }
        return Verdict::Refused;
    }

    void emit(const OrderCmd& cmd, OrderEventType type) noexcept {
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
    const auto venue_ms = parse_server_time(response->body);
    if (!venue_ms.has_value()) {
        return;
    }
    const auto received = state.clock->wall();
    const auto midpoint = sent.ns + (received.ns - sent.ns) / 2;
    state.clock_offset_ns =
        static_cast<std::int64_t>(*venue_ms * 1'000'000ULL) - static_cast<std::int64_t>(midpoint);
}

class OkxInstruments final : public exec::InstrumentSource {
public:
    explicit OkxInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto inst_type = market == MarketType::Spot ? "SPOT" : "SWAP";
        const auto response =
            await_response(*state_, rest_, state_->builder.instruments(inst_type), 20'000);
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
        if (envelope->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
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
                continue; // not live, not this market, or missing a step
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

// Credentials arrive as environment-variable NAMES; a reference that does not
// resolve leaves the field empty, and the adapter then runs market-data only
// rather than signing with half a key.
void load_credentials(VenueState& state, const exec::VenueConfig& config) {
    config::DefaultSecretProvider provider;
    if (!config.api_key_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                state.signer.set_api_key(key.view());
            }
        }
    }
    if (!config.api_secret_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            (void)provider.resolve(*ref, state.signer.secret());
        }
    }
    // OKX is the venue VenueConfig::passphrase_env exists for: without it
    // every signed request comes back 50104, which reads like a bad secret.
    if (!config.passphrase_env.empty()) {
        if (const auto ref = config::SecretRef::parse(config.passphrase_env); ref.has_value()) {
            (void)provider.resolve(*ref, state.passphrase);
        }
    }
}

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    state->spot_enabled = config.enable_spot;
    state->perp_enabled = config.enable_perp;
    load_credentials(*state, config);

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Okx);
    adapter.data = std::make_unique<OkxData>(state);
    adapter.execution = std::make_unique<OkxExec>(state);
    adapter.instruments = std::make_unique<OkxInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Okx, &make);
}

} // namespace raikiri::venues::okx
