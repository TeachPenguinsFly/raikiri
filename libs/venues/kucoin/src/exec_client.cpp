#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/orders.hpp>
#include <raikiri/venues/kucoin/positions.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::kucoin::detail {

namespace {

// One market family: one host, one signed REST pool, one private socket. Spot
// and futures share nothing but the signing scheme, so nothing is shared here
// either — not the connection, not the paths, not the size units.
class MarketLink final : public net::WsHandler, public Pumped {
public:
    MarketLink(VenueState& state, MarketType market, std::uint32_t stream_id)
        : state_{&state}, market_{market}, stream_id_{stream_id},
          rest_{state.io, net::RestConfig{.host = std::string{rest_host(market)}, .port = "443"}},
          builder_{state.credentials} {
        state_->pumps.push_back(this);
    }

    ~MarketLink() { std::erase(state_->pumps, this); }

    [[nodiscard]] MarketType market() const noexcept { return market_; }

    [[nodiscard]] OrderRequestBuilder& builder() noexcept { return builder_; }

    [[nodiscard]] net::RestClient& rest() noexcept { return rest_; }

    void start() {
        rest_.start();
        started_ = true;
        open_stream();
    }

    void stop() {
        if (client_) {
            client_->stop();
            client_.reset();
        }
        rest_.stop();
        started_ = false;
        reopen_due_ = false;
    }

    [[nodiscard]] bool connected() const noexcept {
        return client_ != nullptr && client_->connected();
    }

    void pump(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        if (reopen_due_ && state_->clock->mono().ns >= reopen_at_ns_) {
            reopen_due_ = false;
            open_stream();
        }
        if (connected() && now.ns >= next_ping_ns_) {
            if (const auto frame = protocol_.ping_frame(); !frame.empty()) {
                (void)client_->send(frame);
            }
            next_ping_ns_ = now.ns + ping_interval_ns_;
        }
    }

    void on_open() override {
        const auto topic = private_order_topic(market_);
        const auto frame = protocol_.private_subscribe_frame(topic);
        if (!frame.has_value() || !client_->send(*frame)) {
            // A socket that carries no order topic reports nothing at all,
            // which is indistinguishable from a quiet account.
            schedule_reopen();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Connected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_private_event(payload);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal
        }
        switch (event->kind) {
        case EventKind::OrderChange: {
            const auto wire = parse_order_change(payload);
            if (!wire.has_value()) {
                return;
            }
            if (const auto out = map_order_change(*wire, state_->instruments, received);
                out.has_value()) {
                state_->sink->on_order_event(*out);
            }
            break;
        }
        case EventKind::Error: {
            if (const auto err = parse_stream_error(payload);
                err.has_value() && is_expired_token(err->code)) {
                schedule_reopen(); // the socket's own backoff would reuse the dead token
            }
            break;
        }
        case EventKind::Bbo:
        case EventKind::Trade:
        case EventKind::Depth:
        case EventKind::MarkPrice:
        case EventKind::FundingRate:
        case EventKind::Welcome:
        case EventKind::Ack:
        case EventKind::Pong:
        case EventKind::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason reason) override {
        if (reason != net::WsCloseReason::Requested) {
            // The token is in the URL and expires, so the reconnect has to
            // start from a fresh bullet rather than from the same target.
            schedule_reopen();
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

private:
    void schedule_reopen() noexcept {
        reopen_due_ = true;
        reopen_at_ns_ = state_->clock->mono().ns + k_reopen_delay_ns;
    }

    void open_stream() noexcept {
        const auto request = builder_.bullet_private(market_, state_->venue_now());
        if (!request.has_value()) {
            report_auth_failure();
            return; // no credentials: retrying cannot produce them
        }
        const auto body = usable_body(await_response(*state_, rest_, *request, 10'000));
        if (!body.has_value()) {
            if (body.error().cls == exec::ErrorClass::AuthFailure) {
                // Retrying a refused key at full speed is how an account earns
                // a lockout, so this one failure does not reschedule.
                report_auth_failure();
                return;
            }
            schedule_reopen();
            return;
        }
        const auto bullet = parse_bullet(*body);
        if (!bullet.has_value()) {
            schedule_reopen();
            return;
        }
        const auto endpoint =
            make_endpoint(bullet->endpoint, bullet->token, state_->next_connect_id());
        if (!endpoint.has_value()) {
            schedule_reopen();
            return;
        }
        if (bullet->ping_interval_ms != 0) {
            ping_interval_ns_ = bullet->ping_interval_ms * 1'000'000ULL;
        }
        next_ping_ns_ = 0;

        if (client_) {
            client_->stop();
        }
        client_ = std::make_unique<net::WsClient>(state_->io,
                                                  net::WsConfig{.host = endpoint->host,
                                                                .port = endpoint->port,
                                                                .target = endpoint->target},
                                                  *this);
        client_->start();
    }

    void report_auth_failure() noexcept {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = stream_id_,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    MarketType market_;
    std::uint32_t stream_id_;
    net::RestClient rest_;
    OrderRequestBuilder builder_;
    Protocol protocol_; // its own, so a private frame never races a public one
    std::unique_ptr<net::WsClient> client_;
    std::uint64_t ping_interval_ns_{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t reopen_at_ns_{0};
    bool reopen_due_{false};
    bool started_{false};
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Order entry. Every refusal is named: a market this venue does not serve, a
// pull it does not offer and a request that failed to build are three different
// facts, and the reconciler acts on the difference.
class KucoinExec final : public exec::ExecutionClient {
public:
    KucoinExec(std::shared_ptr<VenueState> state, MarketType market) : state_{std::move(state)} {
        // One link, because one process serves one market. The list stays a
        // list: the private stream is per market and the loop below does not
        // care how many there are.
        links_.push_back(std::make_unique<MarketLink>(*state_, market, /*stream_id=*/1));
    }

    std::expected<void, exec::VenueError> start() override {
        if (links_.empty()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: no market enabled for order entry")};
        }
        if (!state_->credentials.complete()) {
            // Starting anyway would leave every order to fail one at a time,
            // each one looking like a venue problem.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "kucoin: key, secret and passphrase required")};
        }
        for (auto& link : links_) {
            link->start();
        }
        return {};
    }

    void stop() override {
        for (auto& link : links_) {
            link->stop();
        }
    }

    [[nodiscard]] bool connected() const noexcept override {
        if (links_.empty()) {
            return false;
        }
        // Every enabled market must be live: one silent stream is a market
        // whose fills nobody is hearing.
        for (const auto& link : links_) {
            if (!link->connected()) {
                return false;
            }
        }
        return true;
    }

    // The order path's heartbeat, once per market link. The REST pool drops a
    // connection the venue has already closed only if something sweeps it;
    // nothing did, so an idle gateway wrote its next order into a dead socket
    // and read back an end of stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        for (auto& link : links_) {
            link->rest().keepalive(now);
        }
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
        // KuCoin's cancel-all takes no order list: it would also kill orders
        // outside this batch, which is a different instruction from the one
        // given. One request each is the only faithful reading.
        for (const auto& cmd : cmds) {
            cancel(cmd);
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot: {
            auto* link = link_for(market);
            if (link == nullptr) {
                return std::unexpected{market_not_enabled(market)};
            }
            return spot_open_orders(*link);
        }
        case MarketType::Perp:
        case MarketType::Future: {
            auto* link = link_for(market);
            if (link == nullptr) {
                return std::unexpected{market_not_enabled(market)};
            }
            return futures_open_orders(*link);
        }
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        return std::unexpected{exec::not_implemented("kucoin options order entry")};
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        // Neither host can answer this directly: futures offers no read by
        // clientOid at all, and the spot one demands a symbol this signature
        // does not carry. The active sweep is the only route to the answer.
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
        switch (market) {
        case MarketType::Spot:
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().fills(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local FillBuffer storage;
        const auto wire = parse_fills(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_fill(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            // KuCoin does have a spot wallet, GET /api/v1/accounts on the other
            // host. Refused rather than answered from the futures overview:
            // they are two accounts, and money moves between them by transfer.
            return std::unexpected{exec::not_implemented("kucoin spot balance pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().account_overview(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto wire = parse_account_overview(*body);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        const auto balance = map_account_overview(*wire, state_->venue_now());
        if (!balance.has_value()) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "kucoin: account overview names no currency")};
        }
        // ONE ROW: the overview covers one settle currency and this gateway
        // trades one. A second settle currency is a second wallet and a second
        // process, exactly as a second market is.
        return std::vector<BalanceReport>{*balance};
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        switch (market) {
        case MarketType::Spot:
            // Spot holds balances, not positions. An empty list would be read
            // as "flat", which is a claim about a concept this market lacks.
            return std::unexpected{exec::not_implemented("kucoin spot position pull")};
        case MarketType::Perp:
        case MarketType::Future:
            break;
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("kucoin options order entry")};
        }
        auto* link = link_for(market);
        if (link == nullptr) {
            return std::unexpected{market_not_enabled(market)};
        }
        const auto request = link->builder().positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link->rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local PositionBuffer storage;
        const auto wire = parse_positions(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(wire->size());
        for (const auto& row : *wire) {
            if (auto report = map_position(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    [[nodiscard]] static exec::VenueError build_failed(BuildError err) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
    }

    [[nodiscard]] static exec::VenueError market_not_enabled(MarketType market) noexcept {
        return exec::VenueError::make(exec::ErrorClass::Terminal,
                                      0,
                                      market == MarketType::Spot
                                          ? "kucoin spot is not enabled on this gateway"
                                          : "kucoin futures is not enabled on this gateway");
    }

    [[nodiscard]] MarketLink* link_for(MarketType market) noexcept {
        for (auto& link : links_) {
            const auto owned = link->market();
            const bool futures = market == MarketType::Perp || market == MarketType::Future;
            if (owned == market || (futures && owned == MarketType::Perp)) {
                return link.get();
            }
        }
        return nullptr;
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    futures_open_orders(MarketLink& link) {
        const auto request = link.builder().open_orders(link.market(), {}, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = usable_body(await_response(*state_, link.rest(), *request));
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local OrderBuffer storage;
        const auto wire = parse_futures_orders(*body, storage);
        if (!wire.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
        }
        return collect(*wire);
    }

    // KuCoin answers spot actives one symbol at a time, so the sweep asks which
    // symbols have any before asking for them. Two round trips, but the
    // alternative is a per-instrument loop over the whole catalogue.
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    spot_open_orders(MarketLink& link) {
        const auto listing = link.builder().active_symbols(MarketType::Spot, state_->venue_now());
        if (!listing.has_value()) {
            return std::unexpected{build_failed(listing.error())};
        }
        const auto symbols_body = usable_body(await_response(*state_, link.rest(), *listing));
        if (!symbols_body.has_value()) {
            return std::unexpected{symbols_body.error()};
        }
        static thread_local SymbolBuffer symbol_storage;
        const auto symbols = parse_active_symbols(*symbols_body, symbol_storage);
        if (!symbols.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(symbols.error()))};
        }

        std::vector<exec::OrderStatusReport> reports;
        for (const auto symbol : *symbols) {
            const auto request =
                link.builder().open_orders(MarketType::Spot, symbol, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failed(request.error())};
            }
            const auto body = usable_body(await_response(*state_, link.rest(), *request));
            if (!body.has_value()) {
                // A partial sweep read as a complete one is a book that looks
                // emptier than it is, so the whole answer fails together.
                return std::unexpected{body.error()};
            }
            static thread_local OrderBuffer storage;
            const auto wire = parse_spot_orders(*body, storage);
            if (!wire.has_value()) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(wire.error()))};
            }
            for (const auto& row : *wire) {
                if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::vector<exec::OrderStatusReport> collect(std::span<const WireOrder> rows) const {
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows.size());
        for (const auto& row : rows) {
            if (auto report = map_open_order(row, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        auto* link = link_for(cmd.instrument.market());
        if (link == nullptr) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(
                            exec::ErrorClass::Terminal, 0, "no link for this instrument's market"));
            return;
        }
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? link->builder().create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? link->builder().cancel_order(cmd, state_->instruments, now)
                                   : link->builder().amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            emit_reject(
                cmd,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto exchange_client_order_id = cmd.exchange_client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!link->rest().send(
                *request,
                [this, client_order_id, exchange_client_order_id, instrument, side, qty, kind](
                    net::RestResponse&& response) {
                    on_response(std::move(response),
                                client_order_id,
                                exchange_client_order_id,
                                instrument,
                                side,
                                qty,
                                kind);
                })) {
            // Never left this process, so nothing is in doubt.
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
        (void)unanswered_.add({.client_order_id = client_order_id,
                               .exchange_client_order_id = exchange_client_order_id,
                               .instrument = instrument,
                               .side = side,
                               .qty = qty,
                               .kind = cmd.kind,
                               .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     ExchangeClientOrderId exchange_client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        const auto refuse = [&](const exec::VenueError& error) {
            emit(client_order_id, exchange_client_order_id, instrument, side, qty, kind, error);
        };
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(client_order_id);
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            state_->sink->on_order_error(
                client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            refuse(exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            return; // the private socket carries the lifecycle from here
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code, envelope->msg)) {
            return; // the order was already gone, which is what was asked for
        }
        refuse(exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg));
    }

    void emit_reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        emit(cmd.client_order_id,
             cmd.exchange_client_order_id,
             cmd.instrument,
             cmd.side,
             cmd.qty,
             kind,
             error);
    }

    // A REST refusal never reaches the private socket, so the lifecycle event
    // the OMS is waiting for has to be made here or the order sits open forever.
    //
    // The reason is not optional, and it is demanded HERE rather than in
    // `emit_reject` above: this is the funnel both refusal paths pass through,
    // and the other one holds no OrderCmd to name itself with.
    void emit(ClientOrderId client_order_id,
              ExchangeClientOrderId exchange_client_order_id,
              InstrumentId instrument,
              Side side,
              QtyLots qty,
              Kind kind,
              const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.exchange_client_order_id = exchange_client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    std::shared_ptr<VenueState> state_;
    std::vector<std::unique_ptr<MarketLink>> links_;
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state,
                                                             MarketType market) {
    return std::make_unique<KucoinExec>(std::move(state), market);
}

} // namespace raikiri::venues::kucoin::detail
