#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::deribit::detail {

namespace {

// Distinguishes the private socket's connectivity from the market-data one in
// everything the gateway logs.
constexpr std::uint64_t k_private_stream_id = 1;

// A refusal to build a request is terminal by construction: the same command
// built again produces the same refusal.
exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// The private socket. Deribit authenticates the CONNECTION rather than each
// message: public/auth once, and every private call afterwards travels without
// a token. The subscription that matters is only legal once that succeeded,
// which is why it is sent from the auth reply and nowhere else.
class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        (void)client_->send(
            state_->private_protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);
        const auto frame = state_->private_protocol.auth_frame(state_->signer, state_->venue_now());
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        auth_id_ = frame->id;
        authenticated_ = false;
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto event = parse_event(payload);
        if (!event.has_value()) {
            return;
        }
        if (event->kind == EventKind::Response) {
            const auto response = parse_response(payload);
            if (response.has_value() && response->id == auth_id_) {
                on_auth(*response);
                return;
            }
        }
        handle_frame(*state_, *client_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        authenticated_ = false;
        state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }

private:
    void fail() {
        state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    void on_auth(const WireResponse& response) {
        if (response.failed()) {
            state_->last_error_code = response.error_code;
            fail();
            return;
        }
        authenticated_ = true;
        // One channel per (kind, currency): the whole account in four, and a
        // fill on an instrument nothing subscribed to still arrives.
        std::array<std::array<char, k_max_channel_len>, 4> storage{};
        std::array<Subscription, 4> channels{};
        std::size_t count = 0;
        for (const auto market : {MarketType::Perp, MarketType::Option}) {
            for (const auto currency : k_currencies) {
                const auto channel =
                    user_changes_channel(kind_of(market), currency, storage[count]);
                if (channel.empty()) {
                    fail();
                    return;
                }
                channels[count] = Subscription{.channel = channel};
                ++count;
            }
        }
        const auto frame =
            state_->private_protocol.private_subscribe_frame(std::span{channels}.first(count));
        if (!frame.has_value() || !client_->send(frame->text)) {
            fail();
            return;
        }
        state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                       .stream_id = k_private_stream_id,
                                       .ts = state_->clock->wall()});
    }

    VenueState* state_;
    net::WsClient* client_{nullptr};
    std::int64_t auth_id_{0};
    bool authenticated_{false};
};

// Live order entry.
//
// Order entry rides REST while the LIFECYCLE rides the private socket, and the
// division is deliberate: the socket is the single source of every order event,
// so a fill can never be booked twice by arriving on both. The one thing REST
// synthesizes is a rejection, because an order the venue refused produces no
// push at all — there is nothing for the socket to say about an order that
// never existed.
// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

class DeribitExec final : public exec::ExecutionClient {
public:
    explicit DeribitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          rest_{state_->io,
                net::RestConfig{.host = std::string{rest_host(state_->testnet)}, .port = "443"}},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_},
          builder_{state_->signer} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signer.has_credentials()) {
            // Nothing below can work, and saying so here sends an operator to
            // the config instead of to a venue status page.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::AuthFailure, 0, "deribit: no credentials configured")};
        }
        rest_.start();
        sync_clock(*state_, rest_);
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
        return started_ && client_.connected() && handler_.authenticated();
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
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        std::vector<exec::OrderStatusReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.open_orders(market, currency, state_->venue_now());
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
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                // kind=future answers for the perpetual AND the dated series;
                // the instrument table is what separates the two markets.
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        // The ULID went out as the label and comes back as one, so no local
        // table ties the OMS's id to the venue's — but the lookup is scoped by
        // currency and nothing in an id says which coin it was for.
        for (const auto currency : k_currencies) {
            const auto request = builder_.order_state_by_label(id, currency, state_->venue_now());
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
                continue;
            }
            for (const auto& wire : *orders) {
                auto report = map_open_order(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    return *report;
                }
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "deribit: label not found")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        std::vector<exec::FillReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.user_trades(market, currency, since, state_->venue_now());
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local UserTradeBuffer storage;
            const auto trades = parse_user_trades(*body, storage);
            if (!trades.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(trades.error()))};
            }
            for (const auto& wire : *trades) {
                auto report = map_fill_report(wire, state_->instruments);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        if (const auto refusal = market_refusal(market); refusal.has_value()) {
            return std::unexpected{build_failure(*refusal)};
        }
        const auto now = state_->venue_now();
        std::vector<exec::PositionReport> reports;
        for (const auto currency : k_currencies) {
            const auto request = builder_.positions(market, currency, now);
            if (!request.has_value()) {
                return std::unexpected{build_failure(request.error())};
            }
            const auto body = fetch(*request);
            if (!body.has_value()) {
                return std::unexpected{body.error()};
            }
            static thread_local PositionBuffer storage;
            const auto positions = parse_positions(*body, storage);
            if (!positions.has_value()) {
                return std::unexpected{exec::VenueError::make(
                    exec::ErrorClass::Terminal, 0, to_string(positions.error()))};
            }
            for (const auto& wire : *positions) {
                // Deribit stamps no time on a position row, so the read's own
                // clock is the only honest answer to "as of when".
                auto report = map_position(wire, state_->instruments, now);
                if (report.has_value() && report->instrument.market() == market) {
                    reports.push_back(*report);
                }
            }
        }
        return reports;
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
            return std::unexpected{exec::VenueError::make(
                classify_http(response->status), response->status, "http error")};
        }
        // A JSON-RPC error rides a 200, so the envelope decides, not the
        // status line.
        const auto envelope = parse_response(response->body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
        }
        if (envelope->failed()) {
            return std::unexpected{exec::VenueError::make(
                classify(envelope->error_code), envelope->error_code, envelope->error_message)};
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
        if (!rest_.send(*request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
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
        const auto envelope = parse_response(response.body);
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (http_ok && envelope.has_value() && !envelope->failed()) {
            // The socket announces what actually happened; announcing it here
            // as well is how one fill becomes two. That includes a
            // cancel_by_label that matched NOTHING — its result is a count,
            // and a count of zero means the order was already gone, which the
            // socket has therefore already reported as terminal.
            return;
        }
        if (kind != Kind::Create && envelope.has_value() &&
            is_settled_order_refusal(envelope->error_code)) {
            // The order finished between the decision and the request. The
            // venue is agreeing with a state we had not heard about yet, and a
            // CancelRejected here would be raised against an order the FSM has
            // already closed.
            return;
        }
        if (!envelope.has_value()) {
            emit_reject(cmd,
                        kind,
                        exec::VenueError::make(
                            exec::ErrorClass::Terminal, response.status, "unparseable response"));
            return;
        }
        emit_reject(cmd,
                    kind,
                    exec::VenueError::make(classify(envelope->error_code),
                                           envelope->error_code,
                                           envelope->error_message));
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
    PrivateHandler handler_;
    net::RestClient rest_;
    net::WsClient client_;
    SignedRequestBuilder builder_;
    bool started_{false};
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<DeribitExec>(std::move(state));
}

} // namespace raikiri::venues::deribit::detail
