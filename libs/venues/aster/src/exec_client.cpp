// orders go out over signed v3 rest, the only order transport aster offers; the listen-key stream
// reports the lifecycle and the fills

#include <raikiri/exec/order_gate.hpp>
#include <raikiri/venues/aster/account.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clients.hpp"
#include "rest/api.hpp"
#include "userstream/stream.hpp"

namespace raikiri::venues::aster::internal {

namespace {

// the key lapses at 60 minutes; 30 leaves room for one missed call
constexpr std::uint64_t k_listen_key_interval_ns = 30ULL * 60 * 1'000'000'000ULL;

inline constexpr std::size_t k_max_unanswered = 512;

[[nodiscard]] exec::VenueError spot_refusal() noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, "aster spot is market data only");
}

// holds orders sent over rest and not yet answered, and owns the listen key
class AsterExec final : public exec::ExecutionClient {
public:
    explicit AsterExec(std::shared_ptr<VenueState> state) : state_{std::move(state)}, api_{*state_} {
        state_->tick_hook = [this](UnixNanos now) { tick(now); };
    }

    // the hook lives in state shared with the data client, which may outlive this
    ~AsterExec() override { state_->tick_hook = nullptr; }

    AsterExec(const AsterExec&) = delete;
    AsterExec& operator=(const AsterExec&) = delete;
    AsterExec(AsterExec&&) = delete;
    AsterExec& operator=(AsterExec&&) = delete;

    std::expected<void, exec::VenueError> start() override {
        api_.start();
        started_ = true;
        if (!api_.builder().has_credentials() || state_->market == MarketType::Spot) {
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
        if (api_.builder().has_credentials() && !state_->listen_key.empty()) {
            if (const auto request = api_.builder().listen_key_close(state_->venue_now()); request.has_value()) {
                (void)api_.pull(*request, 2'000);
            }
        }
        state_->listen_key.clear();
        api_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return stream_ != nullptr && stream_->connected(); }

    void poll(UnixNanos now) noexcept override {
        api_.keepalive(now);
        sweep_unanswered();
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto* instrument = tradable(cmd, OrderEventType::Rejected);
        if (instrument == nullptr) {
            return;
        }
        if (const auto allowed = api_.spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::Rejected, allowed.error());
            return;
        }
        const auto request = api_.builder().create_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::Rejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::Rejected);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        const auto* instrument = tradable(cmd, OrderEventType::CancelRejected);
        if (instrument == nullptr) {
            return;
        }
        if (const auto allowed = api_.spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, allowed.error());
            return;
        }
        const auto request = api_.builder().cancel_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::CancelRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::CancelRejected);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        const auto* instrument = tradable(cmd, OrderEventType::ModifyRejected);
        if (instrument == nullptr) {
            return;
        }
        if (const auto allowed = api_.spend(ep::k_order, exec::RateClass::Order); !allowed.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, allowed.error());
            return;
        }
        const auto request = api_.builder().amend_order(cmd, base_of(*instrument), state_->venue_now());
        if (!request.has_value()) {
            emit(cmd, OrderEventType::ModifyRejected, build_failed(request.error()));
            return;
        }
        send_order(*request, cmd, OrderEventType::ModifyRejected);
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        if (market == MarketType::Spot) {
            return std::unexpected{spot_refusal()};
        }
        return api_.open_order_reports(market);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType market) override {
        if (market == MarketType::Spot) {
            return std::unexpected{spot_refusal()};
        }
        return api_.query_order(id, market);
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType market,
                                                                                UnixNanos since) override {
        if (market == MarketType::Spot) {
            return std::unexpected{spot_refusal()};
        }
        return api_.fill_reports(market, since);
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError> balance_reports(MarketType market) override {
        return api_.balance_reports(market);
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError> funding_charges(MarketType market,
                                                                              UnixNanos since) override {
        if (market == MarketType::Spot) {
            return std::unexpected{spot_refusal()};
        }
        return api_.funding_charges(market, since);
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType market) override {
        if (market == MarketType::Spot) {
            return std::unexpected{spot_refusal()};
        }
        return api_.position_reports(market);
    }

    // driven by the data client's poll: the key lapses whether or not an order is working
    void tick(UnixNanos now) noexcept {
        if (!started_ || !api_.builder().has_credentials() || state_->market == MarketType::Spot) {
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
        if (const auto request = api_.builder().listen_key_keepalive(state_->venue_now()); request.has_value()) {
            (void)api_.send(*request, [](net::RestResponse&&) {});
        }
    }

private:
    [[nodiscard]] const Instrument* tradable(const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        if (state_->market == MarketType::Spot) {
            emit(cmd, on_failure, spot_refusal());
            return nullptr;
        }
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (instrument == nullptr) {
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument"));
        }
        return instrument;
    }

    // an unanswered order is reported as a gap; reconciliation closes it
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
        }
    }

    void settle(const OrderCmd& cmd, OrderEventType on_failure, const RestError& err) noexcept {
        if (err.code == k_ok) {
            return; // accepted; the user stream reports what happens next
        }
        if (on_failure == OrderEventType::CancelRejected && is_benign_cancel_failure(err.code)) {
            return; // already filled or gone
        }
        const auto cls = classify(err.code, err.msg);
        if (cls == exec::ErrorClass::RateLimited) {
            api_.penalize();
        }
        emit(cmd, on_failure, exec::VenueError::make(cls, err.code, err.msg));
    }

    std::expected<void, exec::VenueError> open_user_stream() {
        const auto request = api_.builder().listen_key_create(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{build_failed(request.error())};
        }
        const auto body = api_.pull(*request);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        const auto key = parse_listen_key(*body);
        if (!key.has_value()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "listen key missing")};
        }
        state_->listen_key.assign(*key);
        state_->listen_key_due_ns = state_->clock->wall().ns + k_listen_key_interval_ns;
        if (stream_) {
            stream_->stop();
        }
        stream_ = std::make_unique<PrivateStream>(*state_, state_->listen_key);
        stream_->start();
        return {};
    }

    void send_order(const net::RestRequest& request, const OrderCmd& cmd, OrderEventType on_failure) noexcept {
        const auto captured = cmd;
        const auto sent = api_.send(request, [this, captured, on_failure](net::RestResponse&& r) {
            if (r.transport_failed()) {
                // the request may have reached the matcher, so this is no rejection; the sweep handles it
                state_->sink->on_order_error(captured.client_order_id,
                                             exec::VenueError::make(exec::ErrorClass::Connectivity,
                                                                    0,
                                                                    r.error.empty()
                                                                        ? std::string_view{"no response from venue"}
                                                                        : std::string_view{r.error}));
                return;
            }
            (void)unanswered_.resolve(captured.client_order_id);
            // 503 means the venue sent it on and heard nothing back; the outcome is unknown
            if (r.status == 503) {
                state_->sink->on_order_error(
                    captured.client_order_id,
                    exec::VenueError::make(exec::ErrorClass::Retryable, 503, "venue timed out; outcome unknown"));
                return;
            }
            const auto err = parse_rest_error(r.body);
            if (!err.has_value()) {
                emit(captured,
                     on_failure,
                     exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable error body"));
                return;
            }
            settle(captured, on_failure, *err);
        });
        if (!sent) {
            // never left this process, so the refusal is certain
            emit(cmd, on_failure, exec::VenueError::make(exec::ErrorClass::Retryable, 0, "rest queue full"));
            return;
        }
        // recorded here since the callback may never run
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    // OrderEvent has no room for text, so the sink gets the error
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
    exec::InFlightTable unanswered_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<AsterExec>(std::move(state));
}

} // namespace raikiri::venues::aster::internal
