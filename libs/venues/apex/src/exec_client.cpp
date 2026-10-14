// orders go over rest; the private socket reports every state change and fill

#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/apex/endpoints.hpp>
#include <raikiri/venues/apex/trade.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clients.hpp"
#include "state.hpp"

namespace raikiri::venues::apex::internal {

namespace {

constexpr std::uint32_t k_private_stream_id = 1;
constexpr std::size_t k_max_unanswered = 512;
constexpr std::size_t k_max_seen_fills = 4096;
constexpr std::uint64_t k_read_timeout_ms = 10'000;
constexpr std::uint64_t k_account_retry_ns = 30'000'000'000ULL;
constexpr std::uint64_t k_ping_interval_ns = 15'000'000'000ULL;
constexpr std::string_view k_page_size = "100";

[[nodiscard]] exec::VenueError refusal(ErrorClass cls, std::int32_t code, std::string_view why) noexcept {
    return exec::VenueError::make(cls, code, why);
}

[[nodiscard]] std::uint64_t wall_ms(const Clock& clock) noexcept {
    return clock.wall().ns / 1'000'000ULL;
}

[[nodiscard]] std::string_view time_in_force(const OrderCmd& cmd) noexcept {
    if (has_flag(cmd.flags, OrderFlags::PostOnly)) {
        return "POST_ONLY";
    }
    switch (cmd.tif) {
    case TimeInForce::Ioc:
        return "IMMEDIATE_OR_CANCEL";
    case TimeInForce::Fok:
        return "FILL_OR_KILL";
    case TimeInForce::Gtc:
        return "GOOD_TIL_CANCEL";
    }
    return "GOOD_TIL_CANCEL";
}

// what a fill row leaves out, learned from the order rows and the create answers
struct KnownOrder {
    ExchangeClientOrderId client{};
    std::string symbol;
    QtyLots size{};
    QtyLots filled{};
};

class ApexExecution final : public exec::ExecutionClient, private net::WsHandler {
public:
    explicit ApexExecution(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}, .port = "443"}},
          stream_{state_->io, stream_config(), *this} {}

    ~ApexExecution() override { stop(); }

    ApexExecution(const ApexExecution&) = delete;
    ApexExecution& operator=(const ApexExecution&) = delete;

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signing.loaded()) {
            return {}; // market data only; every order is refused
        }
        rest_.start();
        stream_.start();
        started_ = true;
        request_account();
        return {};
    }

    void stop() override {
        if (started_) {
            stream_.stop();
            rest_.stop();
            started_ = false;
        }
    }

    [[nodiscard]] bool connected() const noexcept override { return started_ && stream_.connected() && logged_in_; }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        rest_.keepalive(now);
        const auto mono = state_->clock->mono();
        if (!state_->signing.has_account() && mono.ns - account_asked_.ns > k_account_retry_ns) {
            request_account();
        }
        if (stream_.connected() && mono.ns - pinged_.ns > k_ping_interval_ns) {
            pinged_ = mono;
            std::array<char, 24> ts{};
            const auto [stop, ec] = std::to_chars(ts.data(), ts.data() + ts.size(), wall_ms(*state_->clock));
            std::string frame{R"({"op":"ping","args":[")"};
            frame.append(ts.data(), static_cast<std::size_t>(stop - ts.data())).append(R"("]})");
            (void)stream_.send(frame);
        }
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(mono, state_->order_answer_timeout_ns)) {
            exec::report_unknown_outcome(
                *state_->sink, stale, state_->clock->wall(), "no answer from the venue; outcome unknown");
        }
    }

    void submit(const OrderCmd& cmd) noexcept override {
        const auto request = create_request(cmd);
        if (!request.has_value()) {
            reject(cmd, Kind::Create, request.error());
            return;
        }
        send(cmd, Kind::Create, *request);
    }

    void cancel(const OrderCmd& cmd) noexcept override {
        if (!started_) {
            reject(cmd, Kind::Cancel, refusal(ErrorClass::Terminal, 0, to_string(BuildError::NotSigned)));
            return;
        }
        const auto request = state_->signing.cancel(
            k_cancel_by_client_id, cmd.exchange_client_order_id.view(), wall_ms(*state_->clock));
        if (!request.has_value()) {
            reject(cmd, Kind::Cancel, refusal(ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        send(cmd, Kind::Cancel, *request);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        reject(cmd, Kind::Amend, refusal(ErrorClass::Terminal, 0, "apex has no amend; cancel and replace"));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) override {
        auto body = read(k_open_orders, {});
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        return order_reports(*body);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType) override {
        const std::array<Param, 1> params{{{"id", id.view()}}};
        auto body = read(k_order_by_client_id, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto reports = order_reports(*body);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        if (reports->empty()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: no such order")};
        }
        return reports->front();
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType, UnixNanos since) override {
        std::array<char, 24> since_text{};
        const auto [stop, ec] = std::to_chars(
            since_text.data(), since_text.data() + since_text.size(), since.ns / 1'000'000ULL);
        const std::array<Param, 3> params{
            {{"limit", k_page_size},
             {"page", "0"},
             {"beginTimeInclusive", {since_text.data(), static_cast<std::size_t>(stop - since_text.data())}}}};
        auto body = read(k_fills, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PrivateBuffer>();
        const auto rows = parse_rest_rows(*body, *storage, true);
        if (!rows.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: unreadable fill page")};
        }
        std::vector<exec::FillReport> out;
        for (const auto& wire : rows->fills) {
            const auto* instrument = state_->instrument_of(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto event = map_fill(wire, base_of(*instrument), QtyLots::from_raw(0), state_->clock->wall());
            if (!event.has_value()) {
                continue;
            }
            exec::FillReport report{};
            report.exec_id = event->exec_id;
            report.exchange_order_id = event->exchange_order_id;
            report.exchange_client_order_id = event->exchange_client_order_id;
            report.instrument = event->instrument;
            report.side = event->side;
            report.price = event->price;
            report.qty = event->qty;
            report.fee = event->fee;
            report.liquidity = event->liquidity;
            report.ts_venue = event->ts_event;
            out.push_back(report);
        }
        return out;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError> position_reports(MarketType) override {
        auto body = read(k_account, {});
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<PrivateBuffer>();
        const auto account = parse_account(*body, *storage);
        if (!account.has_value()) {
            // an empty vector would read as flat
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: unreadable account")};
        }
        state_->signing.set_account(account->terms);
        std::vector<exec::PositionReport> out;
        for (const auto& wire : account->positions) {
            const auto* instrument = state_->instrument_of(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            if (auto report = map_position(wire, base_of(*instrument));
                report.has_value() && report->net_qty.raw != 0) {
                out.push_back(*report);
            }
        }
        return out;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    net::WsConfig stream_config() {
        net::WsConfig config{
            .host = std::string{ep::stream_host(state_->testnet)}, .port = "443", .target = std::string{k_ws_private}};
        // the login frame repeats the timestamp the target names
        config.target_for_connect = [this] { return state_->signing.stream_target(wall_ms(*state_->clock)); };
        // an order frame must never reach a later socket
        config.keep_queue_on_close = false;
        return config;
    }

    // the fee rates and account id every l2 order signs
    void request_account() noexcept {
        account_asked_ = state_->clock->mono();
        const auto request = state_->signing.get(k_account, {}, wall_ms(*state_->clock));
        if (!request.has_value()) {
            return;
        }
        (void)rest_.send(*request, [this](net::RestResponse&& response) {
            if (response.transport_failed() || response.status != 200) {
                return;
            }
            static thread_local const auto storage = std::make_unique<PrivateBuffer>();
            if (const auto account = parse_account(response.body, *storage); account.has_value()) {
                state_->signing.set_account(account->terms);
            }
        });
    }

    [[nodiscard]] std::expected<net::RestRequest, exec::VenueError> create_request(const OrderCmd& cmd) {
        if (!started_) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(BuildError::NotSigned))};
        }
        if (cmd.order_type != OrderType::Limit) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: send a limit order with ioc")};
        }
        const auto symbol = state_->symbol_of.find(cmd.instrument.packed);
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (symbol == state_->symbol_of.end() || instrument == nullptr) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: instrument not loaded")};
        }
        const auto& base = base_of(*instrument);
        if (cmd.qty.raw <= 0) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: size must be positive")};
        }
        const auto now_ms = wall_ms(*state_->clock);
        const OrderTerms terms{.symbol = symbol->second,
                               .side = cmd.side,
                               .price = base.ticks_to_price(cmd.price),
                               .size = base.lots_to_qty(cmd.qty),
                               .time_in_force = time_in_force(cmd),
                               .client_id = cmd.exchange_client_order_id.view(),
                               .reduce_only = has_flag(cmd.flags, OrderFlags::ReduceOnly),
                               .now_s = now_ms / 1000};
        const auto request = state_->signing.create_order(terms, now_ms);
        if (!request.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        pending_[std::string{cmd.exchange_client_order_id.view()}] = KnownOrder{
            .client = cmd.exchange_client_order_id, .symbol = symbol->second, .size = cmd.qty};
        return *request;
    }

    void send(const OrderCmd& cmd, Kind kind, const net::RestRequest& request) noexcept {
        if (state_->budget != nullptr) {
            const auto taken = state_->budget->take(request.target, state_->clock->mono(), exec::RateClass::Order);
            if (!taken.has_value()) {
                reject(cmd,
                       kind,
                       refusal(ErrorClass::RateLimited,
                               static_cast<std::int32_t>(taken.error().retry_after_ns / 1'000'000),
                               "local rate budget spent; retry after the stated milliseconds"));
                return;
            }
        }
        if (!rest_.send(request, [this, cmd, kind](net::RestResponse&& response) {
                on_response(std::move(response), cmd, kind);
            })) {
            reject(cmd, kind, refusal(ErrorClass::Terminal, 0, "rest queue full"));
            return;
        }
        // recorded before the callback, which may never run
        (void)unanswered_.add(exec::in_flight_of(cmd, state_->clock->mono()));
    }

    void on_response(net::RestResponse&& response, const OrderCmd& cmd, Kind kind) noexcept {
        if (response.transport_failed()) {
            // the request may have reached the matcher, so this is not a rejection
            state_->sink->on_order_error(cmd.client_order_id,
                                         refusal(ErrorClass::Connectivity,
                                                 0,
                                                 response.error.empty() ? std::string_view{"no response from venue"}
                                                                        : std::string_view{response.error}));
            return;
        }
        (void)unanswered_.resolve(cmd.client_order_id);
        const auto reply = parse_reply(response.body);
        if (!reply.has_value()) {
            reject(cmd, kind, refusal(classify(response.status, {}), response.status, "apex: unreadable reply"));
            return;
        }
        if (reply->ok && response.status == 200) {
            if (kind == Kind::Create && !reply->order_id.empty()) {
                const auto pending = pending_.find(std::string{cmd.exchange_client_order_id.view()});
                if (pending != pending_.end()) {
                    // the socket may have named the order first; what it counted stays
                    const auto [it, inserted] = known_.try_emplace(std::string{reply->order_id}, pending->second);
                    if (!inserted) {
                        it->second.client = pending->second.client;
                        it->second.symbol = pending->second.symbol;
                        it->second.size = pending->second.size;
                    }
                    pending_.erase(pending);
                }
            }
            return; // the private socket reports the state change
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(reply->msg)) {
            return;
        }
        if (kind == Kind::Create) {
            pending_.erase(std::string{cmd.exchange_client_order_id.view()});
        }
        const auto cls = classify(response.status, reply->code);
        reject(cmd, kind, refusal(cls, response.status, reply->msg.empty() ? reply->code : reply->msg));
        if (cls == ErrorClass::AuthFailure) {
            state_->sink->on_connectivity(
                {.state = exec::ConnState::AuthFailed, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
        }
    }

    // the reason goes on on_order_error; the terminal event has no field for it
    void reject(const OrderCmd& cmd, Kind kind, const exec::VenueError& error) noexcept {
        state_->sink->on_order_error(cmd.client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = cmd.instrument;
        event.side = cmd.side;
        event.order_type = cmd.order_type;
        event.tif = cmd.tif;
        event.flags = cmd.flags;
        event.client_order_id = cmd.client_order_id;
        event.exchange_client_order_id = cmd.exchange_client_order_id;
        event.price = cmd.price;
        event.qty = cmd.qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : cmd.qty;
        event.reject_class = error.cls;
        event.ts_event = state_->clock->wall();
        event.ts_init = event.ts_event;
        state_->sink->on_order_event(event);
    }

    // cold path: pumps the reactor until the answer arrives
    [[nodiscard]] std::expected<std::string, exec::VenueError> read(std::string_view path,
                                                                    std::span<const Param> params) {
        if (!started_) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(BuildError::NotSigned))};
        }
        const auto request = state_->signing.get(path, params, wall_ms(*state_->clock));
        if (!request.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        if (state_->budget != nullptr) {
            const auto taken = state_->budget->take(request->target, state_->clock->mono(), exec::RateClass::Poll);
            if (!taken.has_value()) {
                return std::unexpected{refusal(ErrorClass::RateLimited,
                                               static_cast<std::int32_t>(taken.error().retry_after_ns / 1'000'000),
                                               "local rate budget spent; retry after the stated milliseconds")};
            }
        }
        auto response = net::await_response(state_->io, *state_->clock, rest_, *request, k_read_timeout_ms);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{refusal(ErrorClass::Connectivity, 0, "apex: no response")};
        }
        const auto reply = parse_reply(response->body);
        if (response->status != 200 || !reply.has_value() || !reply->ok) {
            const auto code = reply.has_value() ? reply->code : std::string_view{};
            return std::unexpected{refusal(classify(response->status, code), response->status, code)};
        }
        return std::move(response->body);
    }

    [[nodiscard]] std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    order_reports(std::string_view body) {
        static thread_local const auto storage = std::make_unique<PrivateBuffer>();
        const auto rows = parse_rest_rows(body, *storage, false);
        if (!rows.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "apex: unreadable orders")};
        }
        std::vector<exec::OrderStatusReport> out;
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            if (auto report = map_order_report(wire, base_of(*instrument)); report.has_value()) {
                out.push_back(*report);
            }
        }
        return out;
    }

    // ---- private socket

    void on_open() override {
        logged_in_ = false;
        const auto login = state_->signing.login_frame();
        if (!login.empty()) {
            (void)stream_.send(login);
        }
        std::string subscribe{R"({"op":"subscribe","args":[")"};
        subscribe.append(k_ws_topic).append(R"("]})");
        (void)stream_.send(subscribe);
    }

    void on_close(net::WsCloseReason) override {
        logged_in_ = false;
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto frame = parse_private_kind(payload);
        if (!frame.has_value()) {
            return;
        }
        switch (frame->kind) {
        case PrivateKind::Ping:
            if (const auto pong = state_->protocol.pong_frame(frame->value); pong.has_value()) {
                (void)stream_.send(*pong);
            }
            return;
        case PrivateKind::Ack:
            if (frame->value == "login") {
                logged_in_ = frame->success;
                state_->sink->on_connectivity(
                    {.state = frame->success ? exec::ConnState::Connected : exec::ConnState::AuthFailed,
                     .stream_id = k_private_stream_id,
                     .ts = state_->clock->wall()});
            }
            return;
        case PrivateKind::Account:
            // only an authenticated socket gets pushes, whatever its login answer looked like
            logged_in_ = true;
            on_account(payload, frame->snapshot, received);
            return;
        case PrivateKind::Other:
            return;
        }
    }

    void on_account(std::string_view payload, bool snapshot, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<PrivateBuffer>();
        const auto rows = parse_account_push(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        // an order row states the total after this frame's fills; each fill counts up from what came before
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            auto& known = learn(wire, base);
            auto before = parse_lots(wire.cum_fill_size, base).value_or(known.filled).raw;
            for (const auto& fill : rows->fills) {
                if (fill.order_id == wire.id && !seen_fills_.contains(std::string{fill.id})) {
                    before -= parse_lots(fill.size, base).value_or(QtyLots{}).raw;
                }
            }
            known.filled = QtyLots::from_raw(before > 0 ? before : 0);
        }
        for (const auto& wire : rows->fills) {
            on_fill(wire, received);
        }
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.symbol);
            if (instrument == nullptr) {
                continue;
            }
            // a snapshot restates open orders; reconciliation owns them after a reconnect
            if (!snapshot) {
                if (auto event = map_order(wire, base_of(*instrument), received); event.has_value()) {
                    if (const auto known = known_.find(std::string{wire.id});
                        event->exchange_client_order_id.view().empty() && known != known_.end()) {
                        event->exchange_client_order_id = known->second.client;
                    }
                    state_->sink->on_order_event(*event);
                }
            }
            if (wire.status == "FILLED" || wire.status == "CANCELED" || wire.status == "EXPIRED") {
                known_.erase(std::string{wire.id});
            }
        }
    }

    void on_fill(const WireFill& wire, UnixNanos received) noexcept {
        if (wire.id.empty() || !seen_fills_.insert(std::string{wire.id}).second) {
            return;
        }
        if (seen_fills_.size() > k_max_seen_fills) {
            seen_fills_.clear();
        }
        const auto known = known_.find(std::string{wire.order_id});
        if (known == known_.end()) {
            return; // no order row and no create answer names it; the fill pull finds it
        }
        const auto* instrument = state_->instrument_of(wire.symbol.empty() ? std::string_view{known->second.symbol}
                                                                           : wire.symbol);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        const auto qty = parse_lots(wire.size, base);
        if (!qty.has_value()) {
            return;
        }
        auto& order = known->second;
        order.filled = QtyLots::from_raw(order.filled.raw + qty->raw);
        const auto leaves = QtyLots::from_raw(order.size.raw > order.filled.raw ? order.size.raw - order.filled.raw
                                                                                : 0);
        auto event = map_fill(wire, base, leaves, received);
        if (!event.has_value()) {
            return;
        }
        event->exchange_client_order_id = order.client;
        state_->sink->on_order_event(*event);
    }

    KnownOrder& learn(const WireOrder& wire, const InstrumentBase& base) {
        auto [it, inserted] = known_.try_emplace(std::string{wire.id});
        if (inserted) {
            const auto pending = pending_.find(std::string{wire.client_order_id});
            if (pending != pending_.end()) {
                it->second = pending->second;
                pending_.erase(pending);
            }
        }
        if (const auto client = ExchangeClientOrderId::from(wire.client_order_id); client.has_value()) {
            it->second.client = *client;
        }
        if (!wire.symbol.empty()) {
            it->second.symbol = std::string{wire.symbol};
        }
        if (const auto size = parse_lots(wire.size, base); size.has_value() && size->raw > 0) {
            it->second.size = *size;
        }
        return it->second;
    }

    [[nodiscard]] static std::optional<QtyLots> parse_lots(std::string_view text, const InstrumentBase& base) noexcept {
        if (text.empty()) {
            return std::nullopt;
        }
        const auto value = parse_decimal_e9(text);
        if (!value.has_value()) {
            return std::nullopt;
        }
        const auto lots = base.qty_to_lots(*value);
        return lots.has_value() ? std::optional{*lots} : std::nullopt;
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    net::WsClient stream_;
    bool started_{false};
    bool logged_in_{false};
    MonoNanos account_asked_{};
    MonoNanos pinged_{};
    exec::InFlightTable unanswered_{k_max_unanswered};
    // by client id until the venue names the order, then by its order id
    std::unordered_map<std::string, KnownOrder> pending_;
    std::unordered_map<std::string, KnownOrder> known_;
    std::unordered_set<std::string> seen_fills_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ApexExecution>(std::move(state));
}

} // namespace raikiri::venues::apex::internal
