#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::binance::detail {

namespace {

// Keepalive cadence. The key lapses at 60 minutes; 30 leaves room for one
// missed call before anything is lost.
constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Where the socket meets the REST call.
//
// An order goes OUT over REST and comes BACK on the user stream, and nothing
// on either side can tell whether the other happened. This class is the only
// thing that knows both: it holds the table of orders handed over and not yet
// answered, ages them out, and synthesises the gap event that stops an
// unanswered order sitting open forever in the strategy's book.
//
// It also owns the listen key, because the key is what the private socket
// connects with and REST is the only place to mint one: neither half can hold
// it alone.
class BinanceExec final : public exec::ExecutionClient {
public:
    explicit BinanceExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, api_{*state_} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    std::expected<void, exec::VenueError> start() override {
        api_.start();
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
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
        if (state_->signer.has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = api_.builder().listen_key_close(); request.has_value()) {
                (void)api_.pull(*request, 2'000);
            }
        }
        state_->listen_key.clear();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return stream_ != nullptr && stream_->connected();
    }

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
            // UNDEF, not zero: zero leaves would read as "nothing is working",
            // and whether anything is working is exactly what is unknown.
            event.leaves_qty = QtyLots::undef();
            event.ts_event = state_->clock->wall();
            event.ts_init = event.ts_event;
            state_->sink->on_order_event(event);
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
        if (const auto allowed = api_.spend(api_.costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        const auto request =
            api_.builder().create_order(cmd, base_of(*instrument), state_->venue_now());
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
        if (const auto allowed = api_.spend(api_.costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request =
            api_.builder().cancel_order(cmd, base_of(*instrument), state_->venue_now());
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
            api_.builder().amend_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

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

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        return api_.position_reports(market);
    }

    // The listen key's own tick, driven by the data client's poll: the key
    // lapses whether or not an order is working.
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
        if (const auto request = api_.builder().listen_key_keepalive(); request.has_value()) {
            (void)api_.send(*request, [](net::RestResponse&&) {});
        }
    }

private:
    std::expected<void, exec::VenueError> open_user_stream() {
        const auto request = api_.builder().listen_key_create();
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = api_.pull(*request);
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

        if (stream_) {
            stream_->stop();
        }
        stream_ = std::make_unique<PrivateStream>(*state_, state_->listen_key);
        stream_->start();
        return {};
    }

    void send_order(const net::RestRequest& request,
                    const OrderCmd& cmd,
                    OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = api_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
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
                api_.penalize();
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
    RestApi api_;
    std::unique_ptr<PrivateStream> stream_;
    bool started_{false};
    // Orders handed to the transport and not yet answered. NOT `in_flight_`:
    // BinanceData already owns a bool by that name for its listen-key refresh,
    // and two members a reader has to disambiguate by class is one too many.
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BinanceExec>(std::move(state));
}

} // namespace raikiri::venues::binance::detail
