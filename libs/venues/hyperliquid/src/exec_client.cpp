#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/sign.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"

namespace raikiri::venues::hyperliquid::detail {

namespace {

// The public socket is stream 0; /exchange gets its own id so an AuthFailed on
// the signing path is distinguishable from a market-data disconnect.
constexpr std::uint32_t k_exchange_stream_id = 1;

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Live order entry, and the table of what it has not answered yet.
//
// There is no private socket here, and the omission is named rather than
// implied: every write is one signed POST to /exchange, and that POST's own
// reply is the whole verdict — it states resting, filled, or refused. There is
// no lifecycle push to join it to, which is why this venue has no userstream/
// beside its rest/. What remains joint is the silence: a POST whose reply never
// arrives leaves an order whose outcome nothing here can know, and that table
// lives with the sender.
class HyperliquidExec final : public exec::ExecutionClient {
public:
    explicit HyperliquidExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, api_{*state_} {}

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
        api_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        api_.stop();
        started_ = false;
    }

    // No socket to be up or down, so this answers from whether start() got past
    // the address checks and brought the REST pool up.
    [[nodiscard]] bool connected() const noexcept override { return started_; }

    // The order path's heartbeat. The REST pool drops a connection the venue
    // has already closed only if something sweeps it; nothing did, so an idle
    // gateway wrote its next order into a dead socket and read back an end of
    // stream — after the venue had filled it.
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
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

    // Forwarded, not reimplemented. These are reconciliation reads and belong
    // to the REST api; ExecutionClient names them because the gateway's
    // pollers reach a venue through one handle.
    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

private:
    // What a command is asking the venue to do. The three share one path
    // because they share one failure: an unanswered request.
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
                ? api_.builder().create_order(cmd, state_->instruments, asset, nonce)
            : kind == Kind::Cancel
                ? api_.builder().cancel_order(cmd, asset, nonce)
                : api_.builder().modify_order(cmd, state_->instruments, asset, nonce);
        if (!request.has_value()) {
            refuse(cmd, kind, to_string(request.error()));
            return;
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
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
    RestApi api_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HyperliquidExec>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::detail
