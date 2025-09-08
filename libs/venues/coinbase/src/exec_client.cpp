#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/coinbase/book.hpp>
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

#include "clients.hpp"

namespace raikiri::venues::coinbase::detail {

namespace {

// A cancel naming an order Coinbase has already finished.
constexpr int k_http_not_found = 404;

// A refusal to build is terminal by construction: the same command built again
// produces the same refusal. NotSigned is the exception, and it points at the
// config rather than at the venue.
// The body may not parse; the status classifies either way.
exec::VenueError http_failure(const net::RestResponse& response) noexcept {
    const auto message = parse_rest_error(response.body);
    return exec::VenueError::make(classify_http(response.status),
                                  response.status,
                                  message.has_value() ? *message : std::string_view{"http error"});
}

exec::VenueError build_failure(BuildError err) noexcept {
    return exec::VenueError::make(err == BuildError::NotSigned ? exec::ErrorClass::AuthFailure
                                                               : exec::ErrorClass::Terminal,
                                  0,
                                  to_string(err));
}

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

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
            if (auto report =
                    map_open_order(wire, state_->instruments, state_->recall(wire.client_oid));
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
                classify_http(response->status),
                response->status,
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
            // Never left this process, so nothing is in doubt.
            emit_reject(cmd,
                        kind,
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
            if (const auto event = map_order_event(*order,
                                                   state_->instruments,
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
    // Orders handed to the transport and not yet answered.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<CoinbaseExec>(std::move(state));
}

} // namespace raikiri::venues::coinbase::detail
