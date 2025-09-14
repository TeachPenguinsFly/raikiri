#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
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

#include "clients.hpp"

namespace raikiri::venues::hyperliquid::detail {

namespace {

// The public socket is stream 0; /exchange gets its own id so an AuthFailed on
// the signing path is distinguishable from a market-data disconnect.
constexpr std::uint32_t k_exchange_stream_id = 1;

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

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

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
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        // frontendOpenOrders, not openOrders: the plain form omits `isTrigger`,
        // so a dormant conditional reads there as a limit at its trigger price.
        const auto body =
            read_info(*state_, rest_, requests_.frontend_open_orders(state_->address));
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
        const auto body = read_info(*state_, rest_, requests_.clearinghouse_state(state_->address));
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
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, asset, nonce)
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

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
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
        fill.liquidity =
            has_flag(cmd.flags, OrderFlags::PostOnly) ? Liquidity::Maker : Liquidity::Taker;
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

    void
    emit(const OrderCmd& cmd, OrderEventType type, std::uint64_t oid, QtyLots leaves) noexcept {
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
        // The reason was already named by every caller and went only to the
        // auth check below; `OrderEvent` has no room for it, so it reaches the
        // sink here or not at all.
        state_->sink->on_order_error(cmd.client_order_id,
                                     exec::VenueError::make(classify_reason(reason), 0, reason));
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
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HyperliquidExec>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::detail
