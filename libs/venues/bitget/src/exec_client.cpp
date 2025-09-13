#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/orders.hpp>
#include <raikiri/venues/bitget/positions.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitget::detail {

namespace {

// Private order stream. Login FIRST and subscribe only once Bitget answers:
// a subscribe sent before the login lands is refused with 30004 and never
// retried, leaving a socket that is up, silent, and indistinguishable from a
// quiet account.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto login = state_->protocol.login_frame(
            state_->signer, state_->passphrase.view(), state_->venue_now());
        if (!login.has_value() || !client_->send(*login)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
        }
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return; // the bare "pong", and whatever Bitget adds next
        }
        switch (event->kind) {
        case EventKind::LoginAck:
            subscribe_orders();
            state_->sink->on_connectivity(
                {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
            break;
        case EventKind::Orders: {
            static thread_local OrderUpdateBuffer storage;
            const auto updates = parse_order_updates(payload, storage);
            if (!updates.has_value()) {
                return;
            }
            for (const auto& wire : *updates) {
                if (const auto mapped = map_order_update(wire, state_->instruments, received);
                    mapped.has_value()) {
                    state_->sink->on_order_event(*mapped);
                }
            }
            break;
        }
        case EventKind::Error: {
            const auto err = parse_stream_error(payload);
            if (!err.has_value()) {
                return;
            }
            state_->last_error_code = err->code;
            if (is_login_failure(err->code)) {
                // Bitget closes the socket when a login fails, so the
                // reconnect will fail identically until a human intervenes.
                state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                               .stream_id = 1,
                                               .ts = state_->clock->wall()});
            } else {
                ++state_->subscribe_refusals;
            }
            break;
        }
        case EventKind::SubscribeAck:
        case EventKind::Ticker:
        case EventKind::Trade:
        case EventKind::Books:
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
    void subscribe_orders() noexcept {
        const std::array<Subscription, 1> orders{
            Subscription{.inst_type = inst_type(state_->market, state_->testnet),
                         .channel = k_channel_orders,
                         .inst_id = k_all_instruments}};
        if (const auto frame = state_->protocol.subscribe_frame(orders); frame.has_value()) {
            (void)client_->send(frame->text);
        }
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class BitgetExec final : public exec::ExecutionClient {
public:
    explicit BitgetExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          ws_{state_->io,
              net::WsConfig{.host = std::string{stream_host()},
                            .port = std::string{stream_port()},
                            .target = std::string{private_stream_path()}},
              handler_},
          rest_{state_->io, net::RestConfig{.host = std::string{rest_host()}, .port = "443"}} {
        handler_.attach(ws_);
    }

    std::expected<void, exec::VenueError> start() override {
        // The passphrase view is taken here, not in the constructor: it
        // reflects the secret's length at the moment it is read, and start()
        // is the first point at which every credential is certainly resolved.
        builder_.set_credentials(state_->signer, state_->passphrase.view());
        rest_.start();
        sync_clock(*state_, rest_);
        ws_.start();
        return {};
    }

    void stop() override {
        ws_.stop();
        rest_.stop();
    }

    [[nodiscard]] bool connected() const noexcept override { return ws_.connected(); }

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

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    // Bitget has no in-place amend on either API. Mix `modify-order` requires
    // a newClientOid and creates a DIFFERENT order asynchronously; spot only
    // offers `cancel-replace-order`, which does the same. An OrderCmd carries
    // one client id and no second one to give the replacement, so honouring
    // either would leave the OMS tracking an order the venue has cancelled.
    // The caps row says amend_order = false and callers cancel and replace
    // with two ids of their own — which is what Bitget does internally anyway.
    void amend(const OrderCmd& cmd) noexcept override {
        emit(cmd,
             OrderEventType::ModifyRejected,
             exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitget has no in-place amend"));
    }

    void cancel_batch(std::span<const OrderCmd> cmds) noexcept override {
        std::size_t sent = 0;
        while (sent < cmds.size()) {
            const auto remaining = cmds.subspan(sent);
            const auto batch = builder_.cancel_batch(
                remaining, state_->instruments, state_->product(), state_->venue_now());
            if (!batch.has_value() || batch->consumed == 0) {
                // One command the batch cannot express must not strand the
                // rest of the panic cancel behind it.
                cancel(remaining.front());
                ++sent;
                continue;
            }
            std::vector<OrderCmd> members{remaining.begin(),
                                          remaining.begin() +
                                              static_cast<std::ptrdiff_t>(batch->consumed)};
            if (!rest_.send(batch->request, [this, members](net::RestResponse&& response) {
                    on_batch_response(std::move(response), members);
                })) {
                // Never left this process, so nothing is in doubt.
                for (const auto& member : members) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
                }
            } else {
                // ON THE WIRE AND UNANSWERED, every member of it: one lost read
                // leaves the whole batch's outcome unknown, not refused.
                for (const auto& member : members) {
                    (void)unanswered_.add(
                        {.client_order_id = member.client_order_id,
                         .exchange_client_order_id = member.exchange_client_order_id,
                         .instrument = member.instrument,
                         .side = member.side,
                         .qty = member.qty,
                         .kind = member.kind,
                         .sent = state_->clock->mono()});
                }
            }
            sent += batch->consumed;
        }
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows =
            pull_orders(builder_.open_orders(market, state_->product(), state_->venue_now()),
                        market,
                        *storage,
                        body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        auto storage = std::make_unique<OrderUpdateBuffer>();
        std::string body;
        const auto rows = pull_orders(
            builder_.order_by_client_id(id, market, state_->product(), state_->venue_now()),
            market,
            *storage,
            body);
        if (!rows.has_value()) {
            return std::unexpected{rows.error()};
        }
        for (const auto& wire : *rows) {
            if (wire.client_oid != id.view()) {
                continue;
            }
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                return *report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        using Reader =
            std::expected<std::span<const WireFill>, WireError> (*)(std::string_view, FillBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_fills;
            break;
        case MarketType::Perp:
            read = &parse_mix_fills;
            break;
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures fill pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request = builder_.fills(market, state_->product(), since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<FillBuffer>();
        const auto rows = read(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_fill_report(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        switch (market) {
        case MarketType::Perp:
            break;
        case MarketType::Spot:
            // Spot holds coin balances, not per-instrument positions: there is
            // nothing to report against an InstrumentId, and answering "flat"
            // would tell a reconciler the account is empty.
            return std::unexpected{exec::not_implemented("bitget spot position pull")};
        case MarketType::Future:
            return std::unexpected{exec::not_implemented("bitget delivery futures position pull")};
        case MarketType::Option:
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        const auto request = builder_.positions(market, state_->product(), state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        const auto body = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto storage = std::make_unique<PositionBuffer>();
        const auto rows = parse_positions(*body, *storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(rows->size());
        for (const auto& wire : *rows) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel };

    static constexpr std::uint64_t k_pull_timeout_ms = 10'000;

    // Both order pulls answer with the same rows under different envelopes,
    // so the market chooses the reader and names its own refusal in one place.
    //
    // `body` is the CALLER's: every returned view points into it, so a body
    // owned here would be freed before the first row was read.
    std::expected<std::span<const WireOrderUpdate>, exec::VenueError>
    pull_orders(const std::expected<net::RestRequest, BuildError>& request,
                MarketType market,
                OrderUpdateBuffer& storage,
                std::string& body) {
        using Reader = std::expected<std::span<const WireOrderUpdate>, WireError> (*)(
            std::string_view, OrderUpdateBuffer&);
        Reader read = nullptr;
        switch (market) {
        case MarketType::Spot:
            read = &parse_spot_orders;
            break;
        case MarketType::Perp:
            read = &parse_mix_orders;
            break;
        case MarketType::Future:
            // Delivery contracts never reach the instrument table, so every
            // row would map to nothing and the pull would read as "nothing open".
            return std::unexpected{exec::not_implemented("bitget delivery futures order pull")};
        case MarketType::Option:
            // Bitget v2 lists no crypto options; its only options product is
            // US equity options behind the separate v3 UTA API.
            return std::unexpected{exec::not_implemented("bitget options order entry")};
        }

        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        auto fetched = fetch_checked(*state_, rest_, *request, k_pull_timeout_ms);
        if (!fetched.has_value()) {
            return std::unexpected{fetched.error()};
        }
        body = std::move(*fetched);
        const auto rows = read(body, storage);
        if (!rows.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(rows.error()))};
        }
        return *rows;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create
                ? builder_.create_order(cmd, state_->instruments, state_->product(), now)
                : builder_.cancel_order(cmd, state_->instruments, state_->product(), now);
        if (!request.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        const OrderCmd snapshot = cmd; // 128 trivially-copyable bytes
        if (!rest_.send(*request, [this, snapshot, kind](net::RestResponse&& response) {
                on_response(std::move(response), snapshot, kind);
            })) {
            // Never left this process, so nothing is in doubt.
            emit(cmd,
                 refusal_of(kind),
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

    static OrderEventType refusal_of(Kind kind) noexcept {
        // CancelRejected, not Rejected: the order a cancel targets is alive.
        return kind == Kind::Create ? OrderEventType::Rejected : OrderEventType::CancelRejected;
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
        if (!envelope.has_value()) {
            emit(cmd,
                 refusal_of(kind),
                 exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code == k_ok) {
            // The acceptance itself arrives on the private socket; the REST
            // answer carries only ids, and an id without a lifecycle event is
            // a second source of truth nobody reads.
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->code)) {
            return;
        }
        emit(cmd,
             refusal_of(kind),
             exec::VenueError::make(
                 classify(envelope->code), code_number(envelope->code), envelope->msg));
    }

    void on_batch_response(net::RestResponse&& response, const std::vector<OrderCmd>& members) {
        const auto reject_all = [&](const exec::VenueError& error) {
            for (const auto& member : members) {
                emit(member, OrderEventType::CancelRejected, error);
            }
        };
        // Any answer settles it, including a refusal: the table tracks silence.
        if (!response.transport_failed()) {
            for (const auto& member : members) {
                (void)unanswered_.resolve(member.client_order_id);
            }
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The batch may have reached the matcher before
            // the read failed, and calling that a refusal is how a process
            // records a rejection opposite a live position.
            const auto error = exec::VenueError::make(
                exec::ErrorClass::Connectivity,
                0,
                response.error.empty() ? std::string_view{"no response from venue"}
                                       : std::string_view{response.error});
            for (const auto& member : members) {
                state_->sink->on_order_error(member.client_order_id, error);
            }
            return;
        }
        const auto envelope = parse_rest_envelope(response.body);
        if (!envelope.has_value()) {
            reject_all(
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->code != k_ok) {
            if (!is_benign_cancel_failure(envelope->code)) {
                reject_all(exec::VenueError::make(
                    classify(envelope->code), code_number(envelope->code), envelope->msg));
            }
            return;
        }
        // A batch answers 00000 while refusing individual orders inside
        // failureList, so a caller that reads only the envelope believes it
        // cancelled orders that are still working.
        auto storage = std::make_unique<BatchFailureBuffer>();
        const auto failures = parse_batch_failures(response.body, *storage);
        if (!failures.has_value()) {
            return;
        }
        for (const auto& failure : *failures) {
            if (is_benign_cancel_failure(failure.code)) {
                continue;
            }
            for (const auto& member : members) {
                if (member.exchange_client_order_id.view() == failure.client_oid) {
                    emit(member,
                         OrderEventType::CancelRejected,
                         exec::VenueError::make(
                             classify(failure.code), code_number(failure.code), failure.msg));
                }
            }
        }
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
        // A rejected create leaves nothing working; a refused cancel or modify
        // leaves the order exactly as it was.
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
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitgetExec>(std::move(state));
}

} // namespace raikiri::venues::bitget::detail
