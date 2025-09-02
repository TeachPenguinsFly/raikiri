#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/binance_venue.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::binance {

namespace {

// State every binance connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
// Binance settles funding every eight hours and republishes the running rate
// continuously; a poll a minute is far finer than the fact changes.
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // Shared with the account poller: binance counts one pool of weight per IP,
    // so two private buckets would together spend twice the allowance.
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder{signer};

    // The depth seed rides its own pool: a book resynchronising must never
    // queue behind an order, and an order must never queue behind a 40 kB
    // snapshot body.
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet)}}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<std::string> desired_streams;

    // Instruments the gateway asked funding for. The rate is PULLED, not
    // streamed: @markPrice is acknowledged on this venue and then never sends
    // a frame, so a gateway that trusted the stream carried no funding at all.
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // Venue clock offset, measured against /fapi/v1/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

    std::function<void(UnixNanos)> tick_hook;

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send that
// the pool refuses is left alone, because the next delta asks again.
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed()) {
            target.seed_failed();
            return;
        }
        if (const auto err = parse_rest_error(response.body);
            err.has_value() && err->code != k_ok) {
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_depth_snapshot(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        const auto ts_event = ms_to_unix(wire->transaction_ms, response.received);
        if (const auto book = target.snapshot(ts_event, response.received); book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(
            state.builder.depth_snapshot(base.exchange_symbol.view(), k_seed_depth), handled)) {
        book.seed_requested();
    }
}

void handle_depth(VenueState& state, std::string_view data, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_depth_update(data, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // Asked before the delta is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->event_ms, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::DepthUpdate:
        handle_depth(state, *data, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::OrderUpdate:
    case EventKind::AccountUpdate:
    case EventKind::ListenKeyExpired:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        std::vector<std::string_view> streams;
        streams.reserve(state_->desired_streams.size());
        for (const auto& stream : state_->desired_streams) {
            streams.push_back(stream);
        }
        std::size_t sent = 0;
        while (sent < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{streams}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
            sent += frame->consumed;
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

class BinanceData final : public exec::DataClient {
public:
    explicit BinanceData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = "443",
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        state_->depth_rest.stop();
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
        if (kind == exec::SubKind::Funding) {
            for (const auto existing : state_->funding_instruments) {
                if (existing == instrument) {
                    return {};
                }
            }
            state_->funding_instruments.push_back(instrument);
            return {};
        }
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired_streams.size() >= k_max_streams_per_connection) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "stream limit reached")};
        }
        state_->add_stream(stream);
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{stream};
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
        poll_funding(now);
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    // One call covers every symbol, so there is no cursor and no per-symbol
    // fan-out: the whole table refreshes on each poll and only the instruments
    // somebody subscribed reach the sink.
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns ||
            in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.premium_index(),
                    [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
            }
        } catch (...) {
            in_flight_ = false;
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local PremiumBuffer storage;
        const auto rows = parse_premium_index(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            const bool wanted = std::ranges::any_of(state_->funding_instruments,
                                                    [id](InstrumentId sub) { return sub == id; });
            if (!wanted) {
                continue;
            }
            if (const auto funding = map_funding(row, state_->instruments, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
            }
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
    bool in_flight_{false};
};

// Handles one user-stream frame. Kept out of the client so dispatch is
// testable without a socket.
void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case EventKind::OrderUpdate: {
        const auto wire = parse_order_update(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto order_event = map_order_event(*wire, state.instruments, received);
            order_event.has_value()) {
            state.sink->on_order_event(*order_event);
        }
        break;
    }
    case EventKind::ListenKeyExpired:
        state.listen_key_expired = true;
        break;
    case EventKind::AccountUpdate:
        break;
    default:
        break;
    }
}

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
};

// Keepalive cadence. The key lapses at 60 minutes; 30 leaves room for one
// missed call before anything is lost.
constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms = 10'000);

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class BinanceExec final : public exec::ExecutionClient {
public:
    explicit BinanceExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          handler_{*state_} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        started_ = true;
        if (!state_->signer.has_credentials()) {
            return {};
        }
        return open_user_stream();
    }

    void stop() override {
        if (!started_) {
            return; // teardown calls stop twice
        }
        if (client_) {
            client_->stop();
            client_.reset();
        }
        if (state_->signer.has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = state_->builder.listen_key_close(); request.has_value()) {
                (void)await_response(*state_, rest_, *request, 2'000);
            }
        }
        state_->listen_key.clear();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return client_ != nullptr && client_->connected();
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

    [[nodiscard]] exec::RateBudget::Costs costs() const noexcept {
        return state_->budget != nullptr ? state_->budget->costs() : exec::RateBudget::Costs{};
    }

    // ONE POOL. binance meters weight across every endpoint together, so every
    // call here spends `Shared` and only the cost differs — thirty for the
    // income ledger against five for a balance, measured from its own
    // used-weight header rather than read off a page.
    [[nodiscard]] std::expected<void, exec::VenueError> spend(std::uint32_t cost,
                                                              exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
        }
        const auto taken =
            state_->budget->take(exec::RateBucket::Shared, cost, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::RateLimited,
            static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
            "local rate budget spent; retry after the stated milliseconds")};
    }

    void penalize() const noexcept {
        if (state_->budget != nullptr) {
            // The venue's answer outranks our accounting, which has just been
            // proven wrong.
            state_->budget->penalize(
                exec::RateBucket::Shared, state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::Rejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        if (const auto allowed = spend(costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        const auto request =
            state_->builder.create_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::CancelRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        if (const auto allowed = spend(costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request =
            state_->builder.cancel_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd,
                 OrderEventType::ModifyRejected,
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
            return;
        }
        const auto request =
            state_->builder.amend_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType) override {
        const auto request = state_->builder.open_orders(state_->venue_now());
        if (const auto allowed = spend(costs().positions, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto wire = parse_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::OrderStatusReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_open_order(entry, state_->instruments);
                report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType) override {
        if (const auto allowed = spend(costs().positions, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto* instrument = first_known_instrument();
        if (instrument == nullptr) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "query needs a loaded catalogue")};
        }
        const auto request = state_->builder.query_order(
            base_of(*instrument).exchange_symbol.view(), id, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_order(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto report = map_open_order(*wire, state_->instruments);
        if (!report.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unmappable order")};
        }
        return *report;
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType, UnixNanos since) override {
        if (const auto allowed = spend(costs().positions, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto* instrument = first_known_instrument();
        if (instrument == nullptr) {
            return std::vector<exec::FillReport>{};
        }
        const auto request = state_->builder.user_trades(
            base_of(*instrument).exchange_symbol.view(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local TradeBuffer storage;
        const auto wire = parse_user_trades(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            if (const auto report = map_user_trade(entry, state_->instruments);
                report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        if (const auto allowed = spend(costs().balances, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        if (market != MarketType::Perp) {
            // Every path in this adapter targets fapi.binance.com, which is
            // USD-M futures and nothing else. Binance spot balances live on
            // another host behind another API; answering with the futures
            // wallet would report the wrong money under a spot label.
            return std::unexpected{exec::not_implemented("binance spot balance pull")};
        }
        const auto request = state_->builder.balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local BalanceBuffer storage;
        const auto wire = parse_balances(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<BalanceReport> out;
        out.reserve(wire->size());
        for (const auto& entry : *wire) {
            // Most rows map to nothing: binance names every collateral asset
            // it accepts, held or not.
            if (const auto report = map_balance(entry); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    std::expected<std::vector<FundingCharge>, exec::VenueError>
    funding_charges(MarketType, UnixNanos since) override {
        if (const auto allowed = spend(costs().ledger, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto request = state_->builder.funding_income(since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local IncomeBuffer storage;
        const auto rows = parse_income(*body, storage);
        if (!rows.has_value()) {
            // A body we cannot read is NOT an empty ledger: answering "no
            // funding" would book zero against a position that paid some.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable income ledger")};
        }
        std::vector<FundingCharge> charges;
        charges.reserve(rows->size());
        for (const auto& row : *rows) {
            if (row.income_type != "FUNDING_FEE") {
                continue; // the endpoint also states commission and realised pnl
            }
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto amount = parse_decimal_e9(row.income);
            if (!amount.has_value()) {
                continue; // an unreadable figure is not a zero one
            }
            FundingCharge charge{};
            charge.instrument = base_of(*instrument).id;
            charge.amount = *amount;
            charge.settled_at = UnixNanos{row.time_ms * 1'000'000};
            char buf[32];
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, row.tran_id);
            if (ec == std::errc{}) {
                if (auto id =
                        ExecId::from(std::string_view{buf, static_cast<std::size_t>(end - buf)});
                    id.has_value()) {
                    charge.reference = *id;
                }
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType) override {
        if (const auto allowed = spend(costs().positions, exec::RateClass::Poll);
            !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        const auto request = state_->builder.position_risk(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        std::array<WirePositionRisk, 512> storage{};
        const auto wire = parse_position_risk(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
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

    void tick(UnixNanos now) noexcept {
        if (!started_ || !state_->signer.has_credentials()) {
            return;
        }
        if (state_->listen_key_expired) {
            state_->listen_key_expired = false;
            (void)open_user_stream();
            return;
        }
        if (now.ns < state_->listen_key_due_ns || state_->listen_key.empty()) {
            return;
        }
        state_->listen_key_due_ns = now.ns + k_listen_key_interval_ns;
        if (const auto request = state_->builder.listen_key_keepalive(); request.has_value()) {
            (void)rest_.send(*request, [](net::RestResponse&&) {});
        }
    }

private:
    [[nodiscard]] const Instrument* first_known_instrument() const noexcept {
        const auto all = state_->instruments.all();
        return all.empty() ? nullptr : &all.front();
    }

    static exec::VenueError build_failed(BuildError err) noexcept {
        return exec::VenueError::make(err == BuildError::NoCredentials
                                          ? exec::ErrorClass::AuthFailure
                                          : exec::ErrorClass::Terminal,
                                      0,
                                      to_string(err));
    }

    [[nodiscard]] std::expected<std::string, exec::VenueError>
    usable_body(const std::optional<net::RestResponse>& response) const {
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            const auto cls = classify(err->code);
            if (cls == exec::ErrorClass::RateLimited) {
                penalize();
            }
            return std::unexpected{exec::VenueError::make(cls, err->code, err->msg)};
        }
        return response->body;
    }

    std::expected<void, exec::VenueError> open_user_stream() {
        const auto request = state_->builder.listen_key_create();
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto response = await_response(*state_, rest_, *request, 10'000);
        const auto body = usable_body(response);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto key = parse_listen_key(*body);
        if (!key.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "listen key missing")};
        }
        state_->listen_key.assign(*key);
        state_->listen_key_due_ns = state_->clock->mono().ns + k_listen_key_interval_ns;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(
            state_->io,
            net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                          .port = "443",
                          .target = "/ws/" + state_->listen_key},
            handler_);
        client_->start();
        return {};
    }

    void send_order(const net::RestRequest& request,
                    const OrderCmd& cmd,
                    OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = rest_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            // Any answer settles it, including a refusal: the table tracks
            // silence, not failure.
            if (!r.transport_failed()) {
                (void)unanswered_.resolve(captured.client_order_id);
            }
            if (r.transport_failed()) {
                // NOT A REJECTION. The request may have reached the matcher
                // before the read failed, and calling that a refusal is how a
                // process records a rejection opposite a live position. The
                // order stays in the table for the sweep above.
                state_->sink->on_order_error(
                    captured.client_order_id,
                    exec::VenueError::make(exec::ErrorClass::Connectivity,
                                           0,
                                           r.error.empty()
                                               ? std::string_view{"no response from venue"}
                                               : std::string_view{r.error}));
                return;
            }
            const auto err = parse_rest_error(r.body);
            if (!err.has_value()) {
                emit(captured,
                     on_failure,
                     exec::VenueError::make(
                         exec::ErrorClass::Terminal, 0, "unparseable error body"));
                return;
            }
            if (err->code == k_ok) {
                return; // accepted; the user stream reports what happens next
            }
            if (on_failure == OrderEventType::CancelRejected &&
                is_benign_cancel_failure(err->code)) {
                // The order already filled or was already gone.
                return;
            }
            if (classify(err->code) == exec::ErrorClass::RateLimited) {
                penalize();
            }
            emit(captured,
                 on_failure,
                 exec::VenueError::make(classify(err->code), err->code, err->msg));
        });
        if (!sent) {
            // Never left this process, so nothing is in doubt: a refusal here is
            // the truth, not a guess about what the venue did.
            emit(cmd,
                 on_failure,
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
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    PrivateHandler handler_;
    std::unique_ptr<net::WsClient> client_;
    bool started_{false};
    // Orders handed to the transport and not yet answered. NOT `in_flight_`:
    // BinanceData already owns a bool by that name for its listen-key refresh,
    // and two members a reader has to disambiguate by class is one too many.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

std::optional<net::RestResponse> await_response(VenueState& state,
                                                net::RestClient& rest,
                                                const net::RestRequest& request,
                                                std::uint64_t timeout_ms) {
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

class BinanceInstruments final : public exec::InstrumentSource {
public:
    explicit BinanceInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        rest_.start();
        sync_clock(*state_, rest_);

        const auto response = await_response(*state_, rest_, state_->builder.instruments(), 20'000);
        if (!response.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Connectivity, 0, "catalogue: no response before deadline")};
        }
        if (response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
        }
        if (const auto err = parse_rest_error(response->body);
            err.has_value() && err->code != k_ok) {
            return std::unexpected{
                exec::VenueError::make(classify(err->code), err->code, err->msg)};
        }

        static thread_local InstrumentBuffer storage;
        const auto wire = parse_exchange_info(response->body, storage);
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
                continue; // not trading, not this market, or missing a filter
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
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

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
            (void)provider.resolve(*ref, state->signer.secret());
        }
    }

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Binance);
    adapter.data = std::make_unique<BinanceData>(state);
    adapter.execution = std::make_unique<BinanceExec>(state);
    adapter.instruments = std::make_unique<BinanceInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Binance, &make);
}

} // namespace raikiri::venues::binance
