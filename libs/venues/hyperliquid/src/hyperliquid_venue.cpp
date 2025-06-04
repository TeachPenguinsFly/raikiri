#include <raikiri/config/secret.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raikiri::venues::hyperliquid {

namespace {

// The venue closes a connection idle for 60 s and control frames do not count
// toward that timer, so the app ping runs well inside it.
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// The spot catalogue is 326 pairs plus every token behind them in one body, so
// the read that loads it gets more room than a reconciliation pull.
constexpr std::uint64_t k_catalogue_timeout_ms = 20'000;

// The public socket is stream 0; /exchange gets its own id so an AuthFailed on
// the signing path is distinguishable from a market-data disconnect.
constexpr std::uint32_t k_exchange_stream_id = 1;

// Owned as strings: the instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string coin;
};

// State one gateway process shares. Single threaded by contract: the gateway
// loop is the only caller, which is why nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // The wallet the account reads name. Owned as a string because the request
    // builders take views into it, and it is NOT a secret: it travels in a
    // plain unsigned /info body, which is the whole reason those reads work
    // with no key material at all.
    std::string address;

    // InstrumentId -> the asset id an /exchange action names. Built at
    // catalogue load because it is the one number an instrument cannot yield:
    // SymbolId counts the rows this adapter KEPT, and the venue counts the rows
    // it PUBLISHED, delisted ones included.
    std::unordered_map<std::uint64_t, std::int64_t> asset_ids;

    // The secp256k1 key orders are signed with, and the only place raw key
    // material lives in this adapter. Absent for a market-data-only gateway,
    // which is why every build path checks before it signs.
    Wallet wallet;

    // Strictly increasing across the process, because the venue rejects a
    // nonce at or below the smallest of the hundred highest it has seen.
    NonceSource nonces;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    std::uint64_t subscribe_refusals{0};

    void add_subscription(std::string_view channel, std::string_view coin) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.coin == coin) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{coin});
    }
};

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto kind = parse_event(frame);
    if (!kind.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (*kind) {
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
        // One taker sweeping several makers arrives as one frame.
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::AssetCtx: {
        const auto wire = parse_asset_ctx(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Book: {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(frame, storage);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const auto frame =
                state_->protocol.subscribe_frame({.channel = entry.channel, .coin = entry.coin});
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(*frame)) {
                break; // wedged socket; the reconnect retries the whole set
            }
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

class HyperliquidData final : public exec::DataClient {
public:
    explicit HyperliquidData(std::shared_ptr<VenueState> state)
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
            if (const auto frame =
                    state_->protocol.subscribe_frame({.channel = channel, .coin = symbol});
                frame.has_value()) {
                (void)client_.send(*frame);
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

// Cold paths only: catalogue loads and reconciliation pulls, never an order.
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

// A query this venue cannot satisfy comes back HTTP 200 with a body of the bare
// literal `null`, so the status alone cannot tell a refusal from a real read —
// and a caller that trusted it would parse an empty account.
std::expected<std::string, exec::VenueError> read_info(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    if (request.body.empty()) {
        // The builder reports overflow by emitting nothing: a truncated body
        // would be a DIFFERENT query, answered rather than refused.
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "hyperliquid: request body did not fit")};
    }
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Connectivity, 0, "hyperliquid: no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const bool ok_status = response->status >= 200 && response->status < 300;
    if (!ok_status || is_null_body(response->body)) {
        const auto reason = is_null_body(response->body) ? std::string_view{"/info answered null"}
                                                         : std::string_view{response->body};
        return std::unexpected{exec::VenueError::make(
            classify(response->status, response->body), response->status, reason)};
    }
    return response->body;
}

// Asked BEFORE the instrument table is read: find() compares the WHOLE id, so
// an id for a market never loaded here resolves to nothing and would answer
// UnknownInstrument, blaming the catalogue for a market never served. Option is
// a named case so the compiler forces a decision rather than a default arm.
std::optional<exec::VenueError> refuse_market(MarketType market) noexcept {
    switch (market) {
    case MarketType::Spot:
    case MarketType::Perp:
        return std::nullopt;
    case MarketType::Future:
        // The venue lists nothing dated: there are two catalogues, and they
        // are `meta.universe` (perpetuals) and `spotMeta.universe` (pairs).
        return exec::not_implemented("hyperliquid dated futures order entry");
    case MarketType::Option:
        return exec::not_implemented("hyperliquid options order entry");
    }
    return exec::not_implemented("hyperliquid: unknown market");
}

// The account is asked for explicitly and never defaulted to the signing key's
// own address: an approved API wallet signs for a DIFFERENT account by design,
// so guessing would reconcile a wallet holding nothing while trading the one
// that holds everything.
exec::VenueError no_address() noexcept {
    return exec::VenueError::make(
        exec::ErrorClass::AuthFailure, 0, "hyperliquid: no account address configured");
}

exec::VenueError unreadable(WireError err) noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

class HyperliquidExec final : public exec::ExecutionClient {
public:
    explicit HyperliquidExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          builder_{state_->wallet, !state_->testnet} {}

    std::expected<void, exec::VenueError> start() override {
        if (state_->address.empty()) {
            // Loud here beats every pull refusing one at a time, which reads
            // as a broken instrument table rather than a missing config.
            return std::unexpected{no_address()};
        }
        if (state_->address.size() != k_address_chars) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "hyperliquid: account is not an address")};
        }
        // The reads name an address and the writes prove a key; nothing on the
        // wire ties them together. A mismatch reconciles one account while
        // trading another, and the venue reports it as an address neither side
        // recognises. An API wallet signs for a DIFFERENT address by design, so
        // this is a warning about the plain case, not a rule — but the plain
        // case is the one that gets misconfigured.
        if (state_->wallet.loaded() && !same_address(state_->wallet.address(), state_->address)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_exchange_stream_id,
                                           .ts = state_->clock->wall()});
        }
        rest_.start();
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
        if (const auto refusal = refuse_market(market); refusal.has_value()) {
            return std::unexpected{*refusal};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // frontendOpenOrders, not openOrders: the plain form omits `isTrigger`,
        // so a dormant conditional reads there as a limit at its trigger price.
        const auto body = read_info(*state_, rest_, requests_.frontend_open_orders(state_->address));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local OpenOrderBuffer storage;
        const auto orders = parse_open_orders(*body, storage);
        if (!orders.has_value()) {
            return std::unexpected{unreadable(orders.error())};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            // One answer carries both markets — "BTC" beside "@107" — so the
            // filter is the instrument's own market, not a second request.
            auto report = map_open_order(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
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
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        std::array<char, k_cloid_chars> buf{};
        const auto cloid = cloid_text(id, buf);
        if (cloid.empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: client order id is not a ULID")};
        }
        const auto body = read_info(*state_, rest_, requests_.order_status(state_->address, cloid));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        const auto status = parse_order_status(*body);
        if (!status.has_value()) {
            return std::unexpected{unreadable(status.error())};
        }
        if (!status->found) {
            // "unknownOid" is not an error and not a status: an id the venue
            // has forgotten and one it never minted answer identically.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "hyperliquid: order unknown to the venue")};
        }
        const auto report = map_order_status(*status, state_->instruments);
        if (!report.has_value() || report->instrument.market() != market) {
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
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // `startTime` is inclusive milliseconds, and zero is a different
        // request rather than "from the beginning".
        const auto since_ms = since.ns / 1'000'000ULL;
        const auto request = since_ms == 0
                                 ? requests_.user_fills(state_->address)
                                 : requests_.user_fills_by_time(state_->address, since_ms);
        const auto body = read_info(*state_, rest_, request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local FillBuffer storage;
        const auto fills = parse_fills(*body, storage);
        if (!fills.has_value()) {
            return std::unexpected{unreadable(fills.error())};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(fills->size());
        for (const auto& wire : *fills) {
            auto report = map_fill(wire, state_->instruments);
            if (report.has_value() && report->instrument.market() == market) {
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
            // Before the address check: this is a fact about the VENUE, true
            // however the gateway is configured. spotClearinghouseState answers
            // BALANCES, and a balance is not a position — one HYPE holding
            // backs @107 and every other pair quoting it, with no entry price
            // anywhere in the response.
            return std::unexpected{exec::not_implemented("hyperliquid spot position pull")};
        }
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        const auto body =
            read_info(*state_, rest_, requests_.clearinghouse_state(state_->address));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }

        static thread_local PositionBuffer storage;
        const auto positions = parse_positions(*body, storage);
        if (!positions.has_value()) {
            return std::unexpected{unreadable(positions.error())};
        }
        // One `time` for the whole snapshot; there is no per-row timestamp.
        const auto ts = ms_to_unix(storage.ts_ms, state_->clock->wall());
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_position(wire, state_->instruments, ts); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // -1 when this gateway never loaded the market. Not derivable from the
    // instrument: SymbolId counts the rows kept, the venue counts the rows
    // published, and spot lives 10000 above perps.
    [[nodiscard]] std::int64_t asset_of(InstrumentId instrument) const noexcept {
        const auto it = state_->asset_ids.find(instrument.packed);
        return it == state_->asset_ids.end() ? -1 : it->second;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto asset = asset_of(cmd.instrument);
        const auto nonce = state_->nonces.next(state_->clock->wall());
        const auto request =
            kind == Kind::Create
                ? builder_.create_order(cmd, state_->instruments, asset, nonce)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, asset, nonce)
                                   : builder_.modify_order(cmd, state_->instruments, asset, nonce);
        if (!request.has_value()) {
            refuse(cmd, kind, to_string(request.error()));
            return;
        }
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            refuse(cmd, kind, "request not queued");
        }
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
        if (response.transport_failed()) {
            // No verdict existed, so this Rejected is a claim the venue never
            // made — but silence leaves the order in doubt forever.
            refuse(cmd, kind, response.error);
            return;
        }
        static thread_local ExchangeStatusBuffer storage;
        const auto reply = parse_exchange_reply(response.body, storage);
        if (!reply.has_value()) {
            refuse(cmd, kind, to_string(reply.error()));
            return;
        }
        if (!reply->ok) {
            // status "err" is a malformed or unauthorised REQUEST. A REFUSED
            // ORDER is status "ok" with an error inside `statuses`, below.
            refuse(cmd, kind, reply->message);
            return;
        }
        if (reply->statuses.empty()) {
            refuse(cmd, kind, "no status for a single-order action");
            return;
        }
        publish(cmd, kind, reply->statuses.front());
    }

    void publish(const OrderCmd& cmd, Kind kind, const WireExchangeStatus& status) noexcept {
        switch (status.outcome) {
        case ExchangeOutcome::Resting:
            emit(cmd, accepted_type(kind), status.oid, cmd.qty);
            return;
        case ExchangeOutcome::Filled:
            publish_fill(cmd, kind, status);
            return;
        case ExchangeOutcome::Success:
            // A create never answers this, so reading it as an ack would
            // invent an order.
            if (kind == Kind::Create) {
                refuse(cmd, kind, "create answered with a cancel outcome");
                return;
            }
            emit(cmd,
                 kind == Kind::Cancel ? OrderEventType::Canceled : OrderEventType::Updated,
                 0,
                 kind == Kind::Cancel ? QtyLots::from_raw(0) : cmd.qty);
            return;
        case ExchangeOutcome::WaitingForTrigger:
            // Only a conditional answers this, and none is ever sent: an ack
            // would report a dormant order as resting in the book.
            refuse(cmd, kind, "order armed as a trigger, which was not asked for");
            return;
        case ExchangeOutcome::Error:
            if (kind == Kind::Cancel && is_benign_cancel_failure(status.error)) {
                // The cancel arrived late rather than failing; a CancelRejected
                // would leave the OMS holding an order the venue has closed.
                emit(cmd, OrderEventType::Canceled, 0, QtyLots::from_raw(0));
                return;
            }
            refuse(cmd, kind, status.error);
            return;
        case ExchangeOutcome::Unknown:
            refuse(cmd, kind, "unrecognised order outcome");
            return;
        }
    }

    void publish_fill(const OrderCmd& cmd, Kind kind, const WireExchangeStatus& status) noexcept {
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            refuse(cmd, kind, "filled an instrument this gateway does not hold");
            return;
        }
        const auto& base = base_of(*instrument);
        const auto filled = qty_from_wire(status.total_qty, base);
        const auto price = price_from_wire(status.avg_price, base);
        if (!filled.has_value()) {
            refuse(cmd, kind, "fill size does not divide by the lot");
            return;
        }
        // A real ack: it reached the book before it matched, and carries the
        // venue id every later event is keyed by.
        emit(cmd, accepted_type(kind), status.oid, cmd.qty);

        // Clamped at zero: a fill larger than the order should be impossible,
        // and a negative remainder would be read downstream as a partial fill
        // that can never complete.
        const auto leaves =
            QtyLots::from_raw(filled->raw >= cmd.qty.raw ? 0 : cmd.qty.raw - filled->raw);
        OrderEvent fill{};
        populate(fill, cmd);
        fill.type = OrderEventType::Filled;
        fill.exchange_order_id = order_id_of(status.oid);
        // An average across makers need not land on the tick grid, so an
        // unconvertible price is left undefined rather than dropping the fill.
        if (price.has_value()) {
            fill.price = *price;
        }
        fill.qty = *filled;
        fill.leaves_qty = leaves;
        fill.liquidity = has_flag(cmd.flags, OrderFlags::PostOnly) ? Liquidity::Maker
                                                                   : Liquidity::Taker;
        fill.ts_event = state_->clock->wall();
        fill.ts_init = fill.ts_event;
        state_->sink->on_order_event(fill);

        // The remainder of a part-filled IOC rests nowhere and the venue does
        // not say so twice; without this the OMS waits on leaves that are gone.
        if (leaves.raw > 0 &&
            (cmd.tif == TimeInForce::Ioc || cmd.order_type == OrderType::Market)) {
            emit(cmd, OrderEventType::Expired, status.oid, QtyLots::from_raw(0));
        }
    }

    [[nodiscard]] static OrderEventType accepted_type(Kind kind) noexcept {
        return kind == Kind::Amend ? OrderEventType::Updated : OrderEventType::Accepted;
    }

    [[nodiscard]] static ExchangeOrderId order_id_of(std::uint64_t oid) noexcept {
        if (oid == 0) {
            return {};
        }
        std::array<char, 24> digits{};
        const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), oid);
        if (ec != std::errc{}) {
            return {};
        }
        const auto id = ExchangeOrderId::from(
            std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())});
        return id.has_value() ? *id : ExchangeOrderId{};
    }

    void populate(OrderEvent& event, const OrderCmd& cmd) const noexcept {
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.instrument = cmd.instrument;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.price = cmd.price;
        event.qty = cmd.qty;
    }

    void emit(const OrderCmd& cmd,
              OrderEventType type,
              std::uint64_t oid,
              QtyLots leaves) noexcept {
        OrderEvent event{};
        populate(event, cmd);
        event.type = type;
        event.exchange_order_id = order_id_of(oid);
        event.leaves_qty = leaves;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // Through the sink, like every other outcome: nothing is pushed for a
    // REST-level failure, so a returned error would leave the order in doubt.
    void refuse(const OrderCmd& cmd, Kind kind, std::string_view reason) noexcept {
        OrderEvent event{};
        populate(event, cmd);
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        // A create that never reached the book leaves nothing behind; a refused
        // cancel or amend leaves the order exactly as it was.
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);

        // A wrong signature answers "User or API Wallet 0x<a stranger> does
        // not exist", a different stranger each attempt. Nothing this key signs
        // will be taken until a human intervenes, so it is said once and loudly.
        if (classify_reason(reason) == exec::ErrorClass::AuthFailure) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = k_exchange_stream_id,
                                           .ts = state_->clock->wall()});
        }
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    UserRequestBuilder requests_;
    ExchangeBuilder builder_;
    bool started_{false};
};

class HyperliquidInstruments final : public exec::InstrumentSource {
public:
    explicit HyperliquidInstruments(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}} {}

    std::expected<std::vector<Instrument>, exec::VenueError> fetch_all(MarketType market) override {
        // Decided before a socket opens: asking for a market this venue does
        // not list costs nothing.
        switch (market) {
        case MarketType::Perp:
            return load_perp();
        case MarketType::Spot:
            return load_spot();
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("hyperliquid dated futures catalogue")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("hyperliquid options catalogue")};
        }
        return std::unexpected{exec::not_implemented("hyperliquid: unknown market")};
    }

private:
    std::expected<std::vector<Instrument>, exec::VenueError> load_perp() {
        // No clock sync anywhere in this adapter: /info is unauthenticated and
        // the venue publishes no server-time read.
        rest_.start();
        const auto body = read_info(*state_, rest_, RequestBuilder::meta(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local AssetBuffer storage;
        const auto wire = parse_meta(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{unreadable(wire.error())};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_instrument(entry, MarketType::Perp);
            if (!instrument.has_value()) {
                continue; // delisted, or an underivable step
            }
            // The row's position in `universe` IS the asset id. Counting the
            // rows we kept shifts every asset after a delisted one.
            keep(std::move(*instrument), entry.index, loaded);
        }
        return loaded;
    }

    std::expected<std::vector<Instrument>, exec::VenueError> load_spot() {
        rest_.start();
        const auto body =
            read_info(*state_, rest_, RequestBuilder::spot_meta(), k_catalogue_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<SpotAssetBuffer>();
        const auto wire = parse_spot_meta(*body, *storage);
        if (!wire.has_value()) {
            return std::unexpected{unreadable(wire.error())};
        }

        std::vector<Instrument> loaded;
        loaded.reserve(wire->size());
        state_->instruments.reserve(state_->instruments.size() + wire->size());
        for (const auto& entry : *wire) {
            auto instrument = map_spot_instrument(entry);
            if (!instrument.has_value()) {
                continue;
            }
            // A DIFFERENT index space: pair 107 is asset 10107, while asset
            // 107 is an unrelated perpetual. Getting it wrong trades that.
            keep(std::move(*instrument), spot_asset_id(entry.index), loaded);
        }
        return loaded;
    }

    void keep(Instrument instrument, std::int64_t asset, std::vector<Instrument>& loaded) {
        const auto id = state_->instruments.add(std::move(instrument));
        if (!id.has_value()) {
            return; // duplicate or table full: reported by count, not by throw
        }
        state_->asset_ids.emplace(id->packed, asset);
        // The table rewrote the id to the slot that indexes it.
        loaded.push_back(*state_->instruments.find(*id));
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
};

exec::VenueAdapter
make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<VenueState>(sink, clock, config.testnet);
    // The address rides `api_key_env` for want of a better field, and resolves
    // through the secret provider for its schemes rather than for secrecy: an
    // Ethereum address is public by construction.
    if (!config.api_key_env.empty()) {
        config::DefaultSecretProvider provider;
        if (const auto ref = config::SecretRef::parse(config.api_key_env); ref.has_value()) {
            config::Secret address;
            if (provider.resolve(*ref, address).has_value()) {
                state->address = std::string{address.view()};
            }
        }
    }
    // Not an HMAC secret: a secp256k1 private key, and it IS the account. A
    // gateway without it reads the account fine and can place nothing.
    if (!config.api_secret_env.empty()) {
        config::DefaultSecretProvider provider;
        if (const auto ref = config::SecretRef::parse(config.api_secret_env); ref.has_value()) {
            config::Secret key;
            if (provider.resolve(*ref, key).has_value()) {
                (void)state->wallet.load(key.view());
            }
        }
    }
    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Hyperliquid);
    adapter.data = std::make_unique<HyperliquidData>(state);
    adapter.execution = std::make_unique<HyperliquidExec>(state);
    adapter.instruments = std::make_unique<HyperliquidInstruments>(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Hyperliquid, &make);
}

} // namespace raikiri::venues::hyperliquid
