#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/post.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/sign.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"

namespace raikiri::venues::hyperliquid::internal {

namespace {

// own stream id so a signing auth failure is distinct from a market-data disconnect
constexpr std::uint32_t k_exchange_stream_id = 1;

inline constexpr std::size_t k_max_unanswered = 512;

constexpr std::uint32_t k_trade_stream_id = 2;

// a request is never left without a verdict, even where config sets no answer timeout
constexpr std::uint64_t k_default_ws_timeout_ns = 10'000'000'000ULL;

// the venue closes a socket it has not heard from in 60 s
constexpr std::uint64_t k_post_ping_interval_ns = 20'000'000'000ULL;

// no private socket: the /exchange reply is the verdict, and this
// table tracks requests with no reply
class HyperliquidExec final : public exec::ExecutionClient {
public:
    explicit HyperliquidExec(std::shared_ptr<VenueState> state) : state_{std::move(state)}, api_{*state_} {}

    std::expected<void, exec::VenueError> start() override {
        if (state_->address.empty()) {
            return std::unexpected{no_address()};
        }
        if (state_->address.size() != k_address_chars) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::AuthFailure, 0, "hyperliquid: account is not an address")};
        }
        // warn when the signing key's address differs from the account;
        // an api wallet differs by design
        if (state_->wallet.loaded() && !same_address(state_->wallet.address(), state_->address)) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_exchange_stream_id, .ts = state_->clock->wall()});
        }
        api_.start();
        if (state_->wallet.loaded() && !state_->pulls_only) {
            open_trade_socket();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        if (trade_) {
            trade_->stop();
            trade_.reset();
        }
        api_.stop();
        started_ = false;
    }

    // true once start() passed the address checks and the rest pool is up
    [[nodiscard]] bool connected() const noexcept override { return started_; }

    // sweep rest connections the venue closed so an idle gateway does not write into a dead socket
    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        if (trade_) {
            trade_->poll();
        }
        sweep_unanswered();
    }

    // an unanswered order is reported as a gap and left for reconciliation
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
        }
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // -1 when not loaded; the venue's asset id counts published rows,
    // so the instrument cannot yield it
    [[nodiscard]] std::int64_t asset_of(InstrumentId instrument) const noexcept {
        const auto it = state_->asset_ids.find(instrument.packed);
        return it == state_->asset_ids.end() ? -1 : it->second;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto asset = asset_of(cmd.instrument);
        const auto nonce = state_->nonces.next(state_->clock->wall());
        const auto request = kind == Kind::Create ? api_.builder().create_order(cmd, state_->instruments, asset, nonce)
                             : kind == Kind::Cancel
                                 ? api_.builder().cancel_order(cmd, asset, nonce)
                                 : api_.builder().modify_order(cmd, state_->instruments, asset, nonce);
        if (!request.has_value()) {
            refuse(cmd, kind, to_string(request.error()));
            return;
        }
        if (send_ws(cmd, kind, request->body)) {
            return;
        }
        if (!api_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            refuse(cmd, kind, "request not queued");
            return;
        }
        // recorded before the callback, which may never run
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    void open_trade_socket() {
        net::WsRpcConfig config{};
        config.ws.host = std::string{stream_host(state_->testnet)};
        config.ws.port = std::string{stream_port()};
        config.ws.target = std::string{stream_path()};
        config.request_timeout_ns = state_->order_answer_timeout_ns != 0 ? state_->order_answer_timeout_ns
                                                                         : k_default_ws_timeout_ns;
        config.ping_interval_ns = k_post_ping_interval_ns;
        config.max_in_flight = k_post_in_flight;
        trade_ = std::make_unique<net::WsRpc>(state_->io, *state_->clock, std::move(config), codec_);
        trade_->set_admit([this] {
            return state_->budget == nullptr ||
                   state_->budget->take(k_post_budget_target, state_->clock->mono(), exec::RateClass::Order)
                       .has_value();
        });
        trade_->set_observer([this](net::WsRpcState state, std::string_view) {
            if (state == net::WsRpcState::LoggingIn) {
                return;
            }
            state_->sink->on_connectivity({.state = state == net::WsRpcState::Ready ? exec::ConnState::Connected
                                                    : state == net::WsRpcState::AuthFailed
                                                        ? exec::ConnState::AuthFailed
                                                        : exec::ConnState::Disconnected,
                                           .stream_id = k_trade_stream_id,
                                           .ts = state_->clock->wall()});
        });
        trade_->start();
    }

    // false leaves the command for rest with nothing sent
    [[nodiscard]] bool send_ws(const OrderCmd& cmd, Kind kind, std::string_view body) noexcept {
        if (!trade_ || !trade_->ready()) {
            return false;
        }
        const auto id = trade_->next_id();
        const auto frame = posts_.request(id, body);
        if (!frame.has_value()) {
            return false;
        }
        return trade_->request(id, *frame, [this, cmd, kind](const net::WsRpcReply& reply) {
            if (reply.outcome == net::WsRpcOutcome::Unknown) {
                exec::report_unknown_outcome(
                    *state_->sink, exec::in_flight_of(cmd, state_->clock->mono()), state_->clock->wall(), reply.detail);
                return;
            }
            const auto post = parse_post_reply(reply.payload);
            if (!post.has_value()) {
                refuse(cmd, kind, to_string(post.error()));
                return;
            }
            if (post->type != "action") {
                refuse(cmd, kind, post->payload);
                return;
            }
            on_body(post->payload, cmd, kind);
        });
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
        // any answer settles it, including a refusal
        if (!response.transport_failed()) {
            (void)unanswered_.resolve(cmd.client_order_id);
        }
        if (response.transport_failed()) {
            // the request may have reached the matcher, so this is not a rejection
            state_->sink->on_order_error(cmd.client_order_id,
                                         exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                0,
                                                                response.error.empty()
                                                                    ? std::string_view{"no response from venue"}
                                                                    : std::string_view{response.error}));
            return;
        }
        on_body(response.body, cmd, kind);
    }

    // the same verdict whichever transport carried the action
    void on_body(std::string_view body, const OrderCmd& cmd, Kind kind) noexcept {
        static thread_local const auto storage_owner = std::make_unique<ExchangeStatusBuffer>();
        auto& storage = *storage_owner;
        const auto reply = parse_exchange_reply(body, storage);
        if (!reply.has_value()) {
            refuse(cmd, kind, to_string(reply.error()));
            return;
        }
        if (!reply->ok) {
            // "err" is a malformed or unauthorised request; order refusals come in `statuses`
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
            // a create never gets this answer
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
            // only a trigger order answers this, and none is sent
            refuse(cmd, kind, "order armed as a trigger, which was not asked for");
            return;
        case ExchangeOutcome::Error:
            if (kind == Kind::Cancel && is_benign_cancel_failure(status.error)) {
                // the cancel arrived late; the venue has already closed the order
                emit(cmd, OrderEventType::Canceled, 0, QtyLots::from_raw(0));
                return;
            }
            if (kind == Kind::Create && is_ioc_miss(status.error)) {
                // taken in, then cancelled unfilled, as an ioc is elsewhere
                emit(cmd, OrderEventType::Accepted, 0, cmd.qty);
                emit(cmd, OrderEventType::Expired, 0, QtyLots::from_raw(0));
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
        emit(cmd, accepted_type(kind), status.oid, cmd.qty);

        // clamped at zero so an overfill cannot leave a negative remainder
        const auto leaves = QtyLots::from_raw(filled->raw >= cmd.qty.raw ? 0 : cmd.qty.raw - filled->raw);
        OrderEvent fill{};
        populate(fill, cmd);
        fill.type = OrderEventType::Filled;
        fill.exchange_order_id = order_id_of(status.oid);
        // an average price may be off the tick grid; leave it undefined and keep the fill
        if (price.has_value()) {
            fill.price = *price;
        }
        fill.qty = *filled;
        fill.leaves_qty = leaves;
        fill.liquidity = has_flag(cmd.flags, OrderFlags::PostOnly) ? Liquidity::Maker : Liquidity::Taker;
        fill.ts_event = state_->clock->wall();
        fill.ts_init = fill.ts_event;
        state_->sink->on_order_event(fill);

        // an ioc remainder is cancelled without a further message
        if (leaves.raw > 0 && (cmd.tif == TimeInForce::Ioc || cmd.order_type == OrderType::Market)) {
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

    void emit(const OrderCmd& cmd, OrderEventType type, std::uint64_t oid, QtyLots leaves) noexcept {
        OrderEvent event{};
        populate(event, cmd);
        event.type = type;
        event.exchange_order_id = order_id_of(oid);
        event.leaves_qty = leaves;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // reported through the sink since rest failures push nothing
    void refuse(const OrderCmd& cmd, Kind kind, std::string_view reason) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, exec::VenueError::make(classify_reason(reason), 0, reason));
        OrderEvent event{};
        populate(event, cmd);
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        // a refused create leaves nothing; a refused cancel or amend leaves the order unchanged
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);

        // a wrong signature names a random address each attempt; nothing
        // will be accepted until it is fixed
        if (classify_reason(reason) == exec::ErrorClass::AuthFailure) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_exchange_stream_id, .ts = state_->clock->wall()});
        }
    }

    std::shared_ptr<VenueState> state_;
    RestApi api_;
    PostCodec codec_;
    PostFrameBuilder posts_;
    std::unique_ptr<net::WsRpc> trade_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<HyperliquidExec>(std::move(state));
}

} // namespace raikiri::venues::hyperliquid::internal
