#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/account.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bybit::detail {
namespace {

void handle_private_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return;
    }
    switch (topic->kind) {
    case TopicKind::Order: {
        static thread_local OrderUpdateBuffer storage;
        const auto updates = parse_order_updates(frame, storage);
        if (!updates.has_value()) {
            return;
        }
        for (const auto& wire : *updates) {
            if (const auto event = map_order_event(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    case TopicKind::Execution: {
        static thread_local ExecutionBuffer storage;
        const auto executions = parse_executions(frame, storage);
        if (!executions.has_value()) {
            return;
        }
        for (const auto& wire : *executions) {
            // THE LIVE LEG. A settlement reaches the book here, seconds after
            // the venue takes it; the REST ledger pull is the periodic check
            // that this leg missed nothing, not the way the money arrives.
            if (const auto charge = map_funding_charge(wire, state.instruments, received);
                charge.has_value()) {
                state.sink->on_funding_charge(*charge);
                continue;
            }
            if (const auto event = map_execution(wire, state.instruments, received);
                event.has_value()) {
                state.sink->on_order_event(*event);
            }
        }
        break;
    }
    default:
        break;
    }
}

// Cold-path request/response. Every caller of this is a startup or
// reconciliation path, never the hot loop, so pumping the reactor until the
// answer arrives is exactly right: the alternative — an asynchronous
// catalogue load — would force every venue to invent its own half-started
// state, and a gateway that trades before it knows its tick sizes is worse

class PrivateHandler final : public net::WsHandler {
public:
    explicit PrivateHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        const auto auth = state_->protocol.auth_frame(state_->signer, state_->venue_now());
        if (!auth.has_value() || !client_->send(*auth)) {
            state_->sink->on_connectivity({.state = exec::ConnState::AuthFailed,
                                           .stream_id = 1,
                                           .ts = state_->clock->wall()});
            return;
        }
        const std::array<std::string_view, 3> topics{"order", "execution", "position"};
        if (const auto frame = state_->protocol.subscribe_frame(topics); frame.has_value()) {
            (void)client_->send(frame->text);
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Connected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_private_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 1, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BybitExec final : public exec::ExecutionClient {
public:
    explicit BybitExec(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          ws_{state_->io,
              net::WsConfig{.host = std::string{host(state_->testnet)},
                            .port = "443",
                            .target = std::string{private_stream_path()}},
              handler_},
          rest_{
              state_->io,
              net::RestConfig{.host = std::string{ep::rest_host(state_->testnet)}, .port = "443"}},
          builder_{state_->signer} {
        handler_.attach(ws_);
    }

    std::expected<void, exec::VenueError> start() override {
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

    void poll(UnixNanos now) noexcept override {
        rest_.keepalive(now);
        sweep_unanswered();
    }

    // AN ORDER NOBODY ANSWERED IS NOT A REFUSED ORDER. Reported as a gap —
    // audit only, moving no status — because the one thing this process cannot
    // know is whether the venue acted. Reconciliation asks the venue what it
    // holds and is what finally closes these.
    void sweep_unanswered() noexcept {
        if (state_->order_answer_timeout_ns == 0) {
            return;
        }
        for (const auto& stale :
             in_flight_.expire(state_->clock->mono(), state_->order_answer_timeout_ns)) {
            state_->sink->on_order_error(
                stale.client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       "no answer from the venue; outcome unknown"));
            emit_gap(stale);
        }
    }

    void emit_gap(const exec::InFlight& stale) noexcept {
        OrderEvent event{};
        event.type = OrderEventType::ReconGap;
        event.instrument = stale.instrument;
        event.client_order_id = stale.client_order_id;
        event.exchange_client_order_id = stale.exchange_client_order_id;
        event.side = stale.side;
        event.qty = stale.qty;
        // UNDEF, not zero: leaves of zero would read as "nothing is working",
        // and whether anything is working is exactly what is unknown here.
        event.leaves_qty = QtyLots::undef();
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    // Local refusal, before the request exists. RateLimited rather than
    // Terminal: the caller should come back, and `retry_after_ns` says when.
    // A second's hold: long enough that a burst cannot immediately repeat,
    // short enough that one refusal does not stall reconciliation for a minute.
    void penalize(exec::RateBucket bucket) noexcept {
        if (state_->budget != nullptr) {
            state_->budget->penalize(bucket, state_->clock->mono(), 1'000'000'000ULL);
        }
    }

    [[nodiscard]] exec::RateBudget::Costs costs() const noexcept {
        return state_->budget != nullptr ? state_->budget->costs() : exec::RateBudget::Costs{};
    }

    [[nodiscard]] std::expected<void, exec::VenueError>
    spend(exec::RateBucket bucket, std::uint32_t cost, exec::RateClass cls) noexcept {
        if (state_->budget == nullptr) {
            return {}; // unmeasured venue: unlimited, not closed
        }
        const auto taken = state_->budget->take(bucket, cost, state_->clock->mono(), cls);
        if (taken.has_value()) {
            return {};
        }
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::RateLimited,
            static_cast<std::int64_t>(taken.error().retry_after_ns / 1'000'000),
            "local rate budget spent; retry after the stated milliseconds")};
    }

    void submit(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Create); }

    void cancel(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Cancel); }

    void amend(const OrderCmd& cmd) noexcept override { fire(cmd, Kind::Amend); }

    std::expected<std::vector<exec::OrderStatusReport>, exec::VenueError>
    open_order_reports(MarketType market) override {
        const auto request = builder_.open_orders(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local OrderUpdateBuffer storage;
        const auto orders = parse_open_orders(envelope->result, storage);
        if (!orders.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(orders.error()))};
        }
        std::vector<exec::OrderStatusReport> reports;
        reports.reserve(orders->size());
        for (const auto& wire : *orders) {
            if (auto report = map_open_order(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<exec::OrderStatusReport, exec::VenueError>
    query_order(const ExchangeClientOrderId& id, MarketType market) override {
        const auto reports = open_order_reports(market);
        if (!reports.has_value()) {
            return std::unexpected{reports.error()};
        }
        for (const auto& report : *reports) {
            if (report.exchange_client_order_id.view() == id.view()) {
                return report;
            }
        }
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "order not open")};
    }

    std::expected<std::vector<exec::FillReport>, exec::VenueError>
    fill_reports(MarketType market, UnixNanos since) override {
        const auto request = builder_.executions(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local ExecutionBuffer storage;
        const auto executions = parse_executions(envelope->result, storage);
        if (!executions.has_value()) {
            return std::vector<exec::FillReport>{};
        }
        std::vector<exec::FillReport> reports;
        reports.reserve(executions->size());
        for (const auto& wire : *executions) {
            const auto event = map_execution(wire, state_->instruments, state_->clock->wall());
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
            reports.push_back(report);
        }
        return reports;
    }

    std::expected<std::vector<exec::PositionReport>, exec::VenueError>
    position_reports(MarketType market) override {
        const auto request = builder_.positions(market, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().positions);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local PositionBuffer storage;
        const auto positions = parse_position_list(envelope->result, storage);
        if (!positions.has_value()) {
            // NOT an empty account. A body we cannot read says nothing about
            // what is held, and answering with an empty vector here is what
            // turns "could not tell" into "is flat".
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unreadable position list")};
        }
        std::vector<exec::PositionReport> reports;
        reports.reserve(positions->size());
        for (const auto& wire : *positions) {
            if (auto report = map_position(wire, state_->instruments); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

    std::expected<std::vector<LedgerEntry>, exec::VenueError>
    funding_charges(MarketType market, UnixNanos since) override {
        const auto request = builder_.funding_charges(market, since, state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Ledger, costs().ledger);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local LedgerEntryBuffer storage;
        const auto rows = parse_funding_charges(envelope->result, storage);
        if (!rows.has_value()) {
            // A body we cannot read is NOT an empty ledger. Saying "no charges"
            // here would book zero funding against a position that paid some.
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "unreadable transaction log")};
        }
        std::vector<LedgerEntry> charges;
        charges.reserve(rows->size());
        for (const auto& wire : *rows) {
            const auto instrument = state_->instruments.find(wire.symbol);
            if (instrument == nullptr) {
                continue; // a symbol this gateway does not carry
            }
            // ONE ROW CARRIES BOTH COLUMNS, and which one is money depends on
            // the row's type: a TRADE states its fee and leaves funding at
            // zero, a SETTLEMENT the reverse. Reading the wrong column books a
            // zero over a real charge.
            const bool is_fee = wire.type == "TRADE";
            const auto amount = parse_decimal_e9(is_fee ? wire.fee : wire.funding);
            if (!amount.has_value() || amount->raw == 0) {
                continue; // an unparseable or absent figure is not a zero charge
            }
            LedgerEntry charge{};
            charge.instrument = base_of(*instrument).id;
            // bybit states a FEE as a cost: positive is paid away, which is the
            // opposite of how the account moved. Flipped here so both kinds
            // reach the book in one convention.
            charge.amount = is_fee ? FixedE9{-amount->raw} : *amount;
            charge.kind = is_fee ? LedgerKind::Fee : LedgerKind::Funding;
            charge.settled_at = UnixNanos{wire.transaction_ms * 1'000'000};
            // A fee names the trade it was charged on, which is what the
            // comparison keys on; the log's own row id is the fallback.
            const auto reference = is_fee && !wire.trade_id.empty() ? wire.trade_id : wire.id;
            if (auto id = ExecId::from(reference); id.has_value()) {
                charge.reference = *id;
            }
            charge.ts_init = state_->clock->wall();
            charges.push_back(charge);
        }
        return charges;
    }

    std::expected<std::vector<BalanceReport>, exec::VenueError>
    balance_reports(MarketType market) override {
        // ONE WALLET behind both markets: a unified account funds a spot fill
        // and a perp's margin out of the same coins, so the answer does not
        // vary by market and narrowing it to one would be an invention.
        (void)market;
        const auto request = builder_.wallet_balance(state_->venue_now());
        if (!request.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(request.error()))};
        }
        std::string body;
        const auto envelope = fetch(*request, body, exec::RateBucket::Account, costs().balances);
        if (!envelope.has_value()) {
            return std::unexpected{envelope.error()};
        }
        static thread_local CoinBalanceBuffer storage;
        const auto coins = parse_wallet_balance(envelope->result, storage);
        if (!coins.has_value()) {
            // An unreadable wallet is refused rather than reported as an empty
            // vector, which reads as an account holding nothing.
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(coins.error()))};
        }
        const auto ts = ms_to_unix(envelope->time_ms, state_->clock->wall());
        std::vector<BalanceReport> reports;
        reports.reserve(coins->size());
        for (const auto& wire : *coins) {
            if (auto report = map_coin_balance(wire, ts); report.has_value()) {
                reports.push_back(*report);
            }
        }
        return reports;
    }

private:
    enum class Kind : std::uint8_t { Create, Cancel, Amend };

    // The envelope's fields are VIEWS INTO `body`, so the caller owns the
    // body and it outlives every use of them. Returning the two together would
    // leave the views pointing into a string that a move could relocate, and
    // returning the envelope alone — which this did — left them pointing into
    // a response destroyed on the way out. ASan caught it on the first
    // credentialed position pull; nothing without a key ever reaches here.
    [[nodiscard]] std::expected<WireRestEnvelope, exec::VenueError>
    fetch(const net::RestRequest& request,
          std::string& body,
          exec::RateBucket bucket,
          std::uint32_t cost) {
        // Poll class: these are reconciliation pulls, and they may not spend
        // the slice held back for order entry. Refused BEFORE the socket, so
        // the venue never has to say no — its refusals cost more than ours,
        // because a 429 on the order path can arrive after the matcher saw it.
        if (const auto allowed = spend(bucket, cost, exec::RateClass::Poll); !allowed.has_value()) {
            return std::unexpected{allowed.error()};
        }
        auto response = await_response(*state_, rest_, request);
        if (!response.has_value() || response->transport_failed()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response")};
        }
        body = std::move(response->body);
        const auto envelope = parse_envelope(body);
        if (!envelope.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope")};
        }
        if (envelope->ret_code != k_ret_ok) {
            const auto cls = classify(envelope->ret_code);
            if (cls == exec::ErrorClass::RateLimited) {
                // THE VENUE'S ANSWER OUTRANKS OUR ACCOUNTING, which has just
                // been proven wrong. Draining the meter it refused on is what
                // stops the next pass walking into the same wall.
                penalize(bucket);
            }
            return std::unexpected{
                exec::VenueError::make(cls, envelope->ret_code, envelope->ret_msg)};
        }
        return *envelope;
    }

    void fire(const OrderCmd& cmd, Kind kind) noexcept {
        const auto now = state_->venue_now();
        const auto request =
            kind == Kind::Create   ? builder_.create_order(cmd, state_->instruments, now)
            : kind == Kind::Cancel ? builder_.cancel_order(cmd, state_->instruments, now)
                                   : builder_.amend_order(cmd, state_->instruments, now);
        if (!request.has_value()) {
            reject(cmd, to_string(request.error()));
            return;
        }
        if (const auto allowed =
                spend(exec::RateBucket::OrderEntry, costs().order, exec::RateClass::Order);
            !allowed.has_value()) {
            reject(cmd, allowed.error().message.view());
            return;
        }
        const auto client_order_id = cmd.client_order_id;
        const auto instrument = cmd.instrument;
        const auto side = cmd.side;
        const auto qty = cmd.qty;
        if (!rest_.send(
                *request,
                [this, client_order_id, instrument, side, qty, kind](net::RestResponse&& response) {
                    on_response(std::move(response), client_order_id, instrument, side, qty, kind);
                })) {
            reject(cmd, "rest queue full");
            return;
        }
        // ON THE WIRE AND UNANSWERED. Recorded here rather than in the callback
        // because the callback is exactly what may never run.
        (void)in_flight_.add({.client_order_id = cmd.client_order_id,
                              .exchange_client_order_id = cmd.exchange_client_order_id,
                              .instrument = cmd.instrument,
                              .side = cmd.side,
                              .qty = cmd.qty,
                              .kind = cmd.kind,
                              .sent = state_->clock->mono()});
    }

    void on_response(net::RestResponse&& response,
                     ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind) {
        // Any answer at all settles it, including a refusal: what the table
        // tracks is silence, not failure.
        if (!response.transport_failed()) {
            (void)in_flight_.resolve(client_order_id);
        }
        if (response.transport_failed()) {
            // NOT A REJECTION. The request may have reached the matcher before
            // the read failed — that is how this process once recorded a refusal
            // opposite a position bybit really held. The order stays in the
            // table, the desk is told the transport's own words, and the sweep
            // above reports it as a gap if no answer ever arrives.
            state_->sink->on_order_error(
                client_order_id,
                exec::VenueError::make(exec::ErrorClass::Connectivity,
                                       0,
                                       response.error.empty()
                                           ? std::string_view{"no response from venue"}
                                           : std::string_view{response.error}));
            return;
        }
        const auto envelope = parse_envelope(response.body);
        if (!envelope.has_value()) {
            emit_reject(
                client_order_id,
                instrument,
                side,
                qty,
                kind,
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unparseable envelope"));
            return;
        }
        if (envelope->ret_code == k_ret_ok) {
            return;
        }
        if (kind == Kind::Cancel && is_benign_cancel_failure(envelope->ret_code)) {
            return;
        }
        if (classify(envelope->ret_code) == exec::ErrorClass::RateLimited) {
            penalize(exec::RateBucket::OrderEntry);
        }
        emit_reject(client_order_id,
                    instrument,
                    side,
                    qty,
                    kind,
                    exec::VenueError::make(
                        classify(envelope->ret_code), envelope->ret_code, envelope->ret_msg));
    }

    // The reason is not optional. Every refusal here reaches the sink with the
    // verdict that caused it, because the terminal event itself has no room for
    // one and a rejection nobody can explain costs an hour at the exchange.
    void emit_reject(ClientOrderId client_order_id,
                     InstrumentId instrument,
                     Side side,
                     QtyLots qty,
                     Kind kind,
                     const exec::VenueError& error) {
        state_->sink->on_order_error(client_order_id, error);
        OrderEvent event{};
        event.type = kind == Kind::Create   ? OrderEventType::Rejected
                     : kind == Kind::Cancel ? OrderEventType::CancelRejected
                                            : OrderEventType::ModifyRejected;
        event.instrument = instrument;
        event.side = side;
        event.client_order_id = client_order_id;
        event.qty = qty;
        event.leaves_qty = kind == Kind::Create ? QtyLots::from_raw(0) : qty;
        event.ts_init = state_->clock->wall();
        event.ts_event = event.ts_init;
        state_->sink->on_order_event(event);
    }

    void reject(const OrderCmd& cmd, std::string_view why) noexcept {
        emit_reject(cmd.client_order_id,
                    cmd.instrument,
                    cmd.side,
                    cmd.qty,
                    cmd.kind == CmdKind::Cancel  ? Kind::Cancel
                    : cmd.kind == CmdKind::Amend ? Kind::Amend
                                                 : Kind::Create,
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, why));
    }

    std::shared_ptr<VenueState> state_;
    PrivateHandler handler_;
    net::WsClient ws_;
    net::RestClient rest_;
    RequestBuilder builder_;
    // Orders handed to the transport and not yet answered. Sized generously:
    // dropping an entry because the table is full would lose the very fact it
    // exists to keep.
    exec::InFlightTable in_flight_{k_max_unanswered};
};

} // namespace

std::unique_ptr<exec::ExecutionClient> make_execution_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BybitExec>(std::move(state));
}

} // namespace raikiri::venues::bybit::detail
