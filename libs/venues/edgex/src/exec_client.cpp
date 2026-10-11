// orders go over rest; the private socket reports every state change and fill

#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>
#include <raikiri/venues/edgex/trade.hpp>

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

namespace raikiri::venues::edgex::internal {

namespace {

constexpr std::uint32_t k_private_stream_id = 1;
constexpr std::size_t k_max_unanswered = 512;
constexpr std::size_t k_max_seen_fills = 4096;
constexpr std::uint64_t k_read_timeout_ms = 10'000;
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

// what a fill row may leave out, learned from the order rows and the create answers
struct KnownOrder {
    ExchangeClientOrderId client{};
    std::uint32_t contract_id{0};
    std::string side;
    QtyLots size{};
    QtyLots matched{};
};

class EdgexExecution final : public exec::ExecutionClient, private net::WsHandler {
public:
    explicit EdgexExecution(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_perp_rest}, .port = "443"}},
          stream_{state_->io, stream_config(), *this} {}

    ~EdgexExecution() override { stop(); }

    EdgexExecution(const EdgexExecution&) = delete;
    EdgexExecution& operator=(const EdgexExecution&) = delete;

    std::expected<void, exec::VenueError> start() override {
        if (!state_->signing.loaded()) {
            return {}; // market data only; every order is refused
        }
        if (state_->market != MarketType::Perp) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: orders are perps only")};
        }
        rest_.start();
        stream_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        if (started_) {
            stream_.stop();
            rest_.stop();
            started_ = false;
        }
    }

    [[nodiscard]] bool connected() const noexcept override { return started_ && stream_.connected(); }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        rest_.keepalive(now);
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale : unanswered_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
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
        const auto request = state_->signing.cancel_order(cmd.exchange_client_order_id.view(), wall_ms(*state_->clock));
        if (!request.has_value()) {
            reject(cmd, Kind::Cancel, refusal(ErrorClass::Terminal, 0, to_string(request.error())));
            return;
        }
        send(cmd, Kind::Cancel, *request);
    }

    void amend(const OrderCmd& cmd) noexcept override {
        reject(cmd, Kind::Amend, refusal(ErrorClass::Terminal, 0, "edgex has no amend; cancel and replace"));
    }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError> open_order_reports(MarketType) override {
        const std::array<Param, 2> params{{{"accountId", account()}, {"size", k_page_size}}};
        auto body = read(k_active_orders, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        return order_reports(*body);
    }

    std::expected<exec::OrderStatusReport, exec::VenueError> query_order(const ExchangeClientOrderId& id,
                                                                         MarketType) override {
        const std::array<Param, 2> params{{{"accountId", account()}, {"clientOrderIdList", id.view()}}};
        auto body = read(k_order_by_client_id, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        auto reports = order_reports(*body);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        if (reports->empty()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: no such order")};
        }
        return reports->front();
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError> fill_reports(MarketType, UnixNanos since) override {
        std::array<char, 24> since_text{};
        const auto [stop, ec] = std::to_chars(
            since_text.data(), since_text.data() + since_text.size(), since.ns / 1'000'000ULL);
        const std::array<Param, 3> params{{{"accountId", account()},
                                           {"filterStartCreatedTimeInclusive",
                                            {since_text.data(), static_cast<std::size_t>(stop - since_text.data())}},
                                           {"size", k_page_size}}};
        auto body = read(k_fill_page, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<TradeBuffer>();
        const auto rows = parse_rest_rows(*body, *storage, true);
        if (!rows.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: unreadable fill page")};
        }
        std::vector<exec::FillReport> out;
        for (const auto& wire : rows->fills) {
            const auto* instrument = state_->instrument_of(wire.contract_id);
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
        const std::array<Param, 1> params{{{"accountId", account()}}};
        auto body = read(k_account_asset, params);
        if (!body.has_value()) {
            return std::unexpected{body.error()};
        }
        static thread_local const auto storage = std::make_unique<TradeBuffer>();
        const auto rows = parse_account_asset(*body, *storage);
        if (!rows.has_value()) {
            // an empty vector would read as flat
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: unreadable account asset")};
        }
        std::vector<exec::PositionReport> out;
        for (const auto& wire : rows->positions) {
            const auto* instrument = state_->instrument_of(wire.contract_id);
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
            .host = std::string{ep::k_perp_stream}, .port = "443", .target = std::string{k_private_ws}};
        // headers are asked first, so the target repeats their timestamp
        config.headers_for_connect = [this] { return state_->signing.stream_headers(wall_ms(*state_->clock)); };
        config.target_for_connect = [this] { return state_->signing.stream_target(); };
        // an order frame must never reach a later socket
        config.keep_queue_on_close = false;
        return config;
    }

    [[nodiscard]] std::string_view account() const noexcept {
        const auto id = state_->signing.account_id();
        const auto [stop, ec] = std::to_chars(account_text_.data(), account_text_.data() + account_text_.size(), id);
        return {account_text_.data(), static_cast<std::size_t>(stop - account_text_.data())};
    }

    [[nodiscard]] std::expected<net::RestRequest, exec::VenueError> create_request(const OrderCmd& cmd) {
        if (!started_) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(BuildError::NotSigned))};
        }
        if (cmd.order_type != OrderType::Limit) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: send a limit order with ioc")};
        }
        const auto contract = state_->contract_of.find(cmd.instrument.packed);
        const auto* instrument = state_->instruments.find(cmd.instrument);
        if (contract == state_->contract_of.end() || instrument == nullptr) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: instrument not loaded")};
        }
        const auto& base = base_of(*instrument);
        std::array<char, 40> price_text{};
        std::array<char, 40> size_text{};
        const auto price = format_e9(base.ticks_to_price(cmd.price), price_text.data(), price_text.size());
        const auto size = format_e9(base.lots_to_qty(cmd.qty), size_text.data(), size_text.size());
        if (price.empty() || size.empty() || cmd.qty.raw <= 0) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, "edgex: price or size unformattable")};
        }
        const OrderTerms terms{.account_id = state_->signing.account_id(),
                               .contract_id = contract->second,
                               .side = cmd.side,
                               .price = price,
                               .size = size,
                               .time_in_force = time_in_force(cmd),
                               .client_order_id = cmd.exchange_client_order_id.view(),
                               .reduce_only = has_flag(cmd.flags, OrderFlags::ReduceOnly),
                               .now_ms = wall_ms(*state_->clock)};
        const auto request = state_->signing.create_order(terms);
        if (!request.has_value()) {
            return std::unexpected{refusal(ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        remember_pending(cmd, contract->second);
        return *request;
    }

    void remember_pending(const OrderCmd& cmd, std::uint32_t contract_id) {
        pending_[std::string{cmd.exchange_client_order_id.view()}] = KnownOrder{.client = cmd.exchange_client_order_id,
                                                                                .contract_id = contract_id,
                                                                                .side = cmd.side == Side::Buy ? "BUY"
                                                                                                              : "SELL",
                                                                                .size = cmd.qty};
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
            reject(cmd, kind, refusal(classify(response.status, {}), response.status, "edgex: unreadable reply"));
            return;
        }
        const bool ok = reply->code == "SUCCESS" &&
                        (kind != Kind::Cancel || reply->cancel_result.empty() || reply->cancel_result == "SUCCESS");
        if (ok) {
            if (kind == Kind::Create && !reply->order_id.empty()) {
                const auto pending = pending_.find(std::string{cmd.exchange_client_order_id.view()});
                if (pending != pending_.end()) {
                    // the socket may have named the order first; what it counted stays
                    const auto [it, inserted] = known_.try_emplace(std::string{reply->order_id}, pending->second);
                    if (!inserted) {
                        it->second.client = pending->second.client;
                        it->second.contract_id = pending->second.contract_id;
                        it->second.side = pending->second.side;
                        it->second.size = pending->second.size;
                    }
                    pending_.erase(pending);
                }
            }
            return; // the private socket reports the state change
        }
        const auto code = reply->code != "SUCCESS" ? reply->code : reply->cancel_result;
        if (kind == Kind::Cancel && is_benign_cancel_failure(code)) {
            return;
        }
        if (kind == Kind::Create) {
            pending_.erase(std::string{cmd.exchange_client_order_id.view()});
        }
        const auto cls = classify(response.status, code);
        reject(cmd, kind, refusal(cls, response.status, reply->msg.empty() ? code : reply->msg));
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
            return std::unexpected{refusal(ErrorClass::Connectivity, 0, "edgex: no response")};
        }
        if (response->status != 200) {
            const auto reply = parse_reply(response->body);
            const auto code = reply.has_value() ? reply->code : std::string_view{};
            return std::unexpected{refusal(classify(response->status, code), response->status, code)};
        }
        return std::move(response->body);
    }

    [[nodiscard]] std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    order_reports(std::string_view body) {
        static thread_local const auto storage = std::make_unique<TradeBuffer>();
        const auto rows = parse_rest_rows(body, *storage, false);
        if (!rows.has_value()) {
            const auto reply = parse_reply(body);
            return std::unexpected{
                refusal(ErrorClass::Terminal,
                        0,
                        reply.has_value() ? reply->code : std::string_view{"edgex: unreadable orders"})};
        }
        std::vector<exec::OrderStatusReport> out;
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.contract_id);
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

    void on_open() override {}

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto frame = parse_private_kind(payload);
        if (!frame.has_value()) {
            return;
        }
        switch (frame->kind) {
        case PrivateKind::Connected:
            state_->sink->on_connectivity(
                {.state = exec::ConnState::Connected, .stream_id = k_private_stream_id, .ts = state_->clock->wall()});
            return;
        case PrivateKind::Ping:
            if (const auto time = parse_ping(payload); time.has_value()) {
                if (const auto pong = pong_.pong_frame(*time); pong.has_value()) {
                    (void)stream_.send(*pong);
                }
            }
            return;
        case PrivateKind::Trade:
            on_trade_event(payload, frame->event == "Snapshot", received);
            return;
        case PrivateKind::Error:
        case PrivateKind::Other:
            return;
        }
    }

    void on_trade_event(std::string_view payload, bool snapshot, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<TradeBuffer>();
        const auto rows = parse_trade_event(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        // an order row states the total after this frame's fills; each fill counts up from what came before
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.contract_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            auto& known = learn(wire, base);
            auto before = parse_lots(wire.cum_match_size, base).value_or(known.matched).raw;
            for (const auto& fill : rows->fills) {
                if (fill.order_id == wire.id && !seen_fills_.contains(std::string{fill.id})) {
                    before -= parse_lots(fill.size, base).value_or(QtyLots{}).raw;
                }
            }
            known.matched = QtyLots::from_raw(before > 0 ? before : 0);
        }
        for (const auto& wire : rows->fills) {
            on_fill(wire, received);
        }
        for (const auto& wire : rows->orders) {
            const auto* instrument = state_->instrument_of(wire.contract_id);
            if (instrument == nullptr) {
                continue;
            }
            // a snapshot restates open orders; reconciliation owns them after a reconnect
            if (!snapshot) {
                if (auto event = map_order(wire, base_of(*instrument), received); event.has_value()) {
                    // a row without clientOrderId is named by the create answer
                    if (const auto known = known_.find(std::string{wire.id});
                        event->exchange_client_order_id.view().empty() && known != known_.end()) {
                        event->exchange_client_order_id = known->second.client;
                    }
                    state_->sink->on_order_event(*event);
                }
            }
            if (wire.status == "FILLED" || wire.status == "CANCELED") {
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
        const auto* instrument = state_->instrument_of(wire.contract_id != 0 ? wire.contract_id
                                                                             : known->second.contract_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        const auto qty = parse_lots(wire.size, base);
        if (!qty.has_value()) {
            return;
        }
        auto& order = known->second;
        order.matched = QtyLots::from_raw(order.matched.raw + qty->raw);
        const auto leaves = QtyLots::from_raw(order.size.raw > order.matched.raw ? order.size.raw - order.matched.raw
                                                                                 : 0);
        WireFill complete = wire;
        if (complete.side.empty()) {
            complete.side = order.side;
        }
        auto event = map_fill(complete, base, leaves, received);
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
        if (wire.contract_id != 0) {
            it->second.contract_id = wire.contract_id;
        }
        if (!wire.side.empty()) {
            it->second.side = std::string{wire.side};
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
    Protocol pong_;
    bool started_{false};
    exec::InFlightTable unanswered_{k_max_unanswered};
    mutable std::array<char, 24> account_text_{};
    // by clientOrderId until the venue names the order, then by its order id
    std::unordered_map<std::string, KnownOrder> pending_;
    std::unordered_map<std::string, KnownOrder> known_;
    std::unordered_set<std::string> seen_fills_;
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<EdgexExecution>(std::move(state));
}

} // namespace raikiri::venues::edgex::internal
