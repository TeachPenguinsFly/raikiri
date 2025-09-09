#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitmex::detail {

namespace {

// BitMEX answers the bare text "ping" with "pong" and drops a connection it has
// heard nothing from; the banner reports heartbeatEnabled:false, so this is ours.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// Rows in one frame always share a symbol: the subscription is symbol-scoped,
// which is why the instrument is resolved from the first row the way the
// instrument stream reads data[0].
void handle_book(VenueState& state,
                 FrameAction action,
                 std::string_view frame,
                 UnixNanos received) {
    static thread_local LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    if (!wire.has_value() || wire->empty()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->front().symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.book_for(base);
    const auto result = book.apply(action, *wire);
    if (result != BookResult::Applied && result != BookResult::Resynced) {
        return; // unsynced or diverged: silence, never a guess
    }
    // The frame root carries no time on this table; the rows each carry one.
    if (const auto depth = book.snapshot(iso8601_to_unix(wire->front().ts, received), received);
        depth.has_value()) {
        state.sink->on_depth(*depth);
    }
}

void handle_quotes(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local QuoteBuffer quotes;
    const auto wire = parse_quotes(frame, quotes);
    if (!wire.has_value()) {
        return;
    }
    // A `partial` carries one row per pool; only the last completes the book.
    for (const auto& row : *wire) {
        const auto* instrument = state.instruments.find(row.symbol);
        if (instrument == nullptr) {
            continue;
        }
        const auto& base = base_of(*instrument);
        if (const auto bbo = state.state_of(base.id).quotes.apply(row, base, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        // Includes the bare "pong": unknown shapes are ignored, never fatal.
        return;
    }
    switch (info->table) {
    case TableKind::Quote:
        handle_quotes(state, frame, received);
        break;
    case TableKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        // Dropping the tail would lose real volume on a busy contract.
        for (const auto& row : *wire) {
            if (const auto trade = map_trade(row, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case TableKind::Instrument: {
        const auto wire = parse_instrument_tick(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(wire->symbol);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        if (const auto funding = state.state_of(base.id).instrument.apply(*wire, base, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case TableKind::OrderBook:
        handle_book(state, info->action, frame, received);
        break;
    case TableKind::Error: {
        // Terminal for the arg it names, and silence is how it would otherwise present.
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_status = err->status;
            ++state.subscribe_refusals;
        }
        break;
    }
    case TableKind::SubscribeAck:
    case TableKind::Info:
    case TableKind::Unknown:
        break;
    // Private tables are never subscribed on the market-data socket. Naming
    // them here rather than letting a default swallow them is what makes a new
    // private table a compiler error instead of a silently dropped frame.
    case TableKind::Order:
    case TableKind::Execution:
    case TableKind::Position:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        // The ONE resubscribe path, which is why the transport tracks none of it.
        std::vector<Subscription> views;
        views.reserve(state_->desired.size());
        for (const auto& entry : state_->desired) {
            views.push_back({.table = entry.table, .symbol = entry.symbol});
        }
        std::size_t sent = 0;
        while (sent < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
            sent += frame->consumed;
        }
        // The ids in the old book name levels of a subscription that no longer
        // exists; the fresh `partial` is the only thing that may repopulate it.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BitmexData final : public exec::DataClient {
public:
    explicit BitmexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return client_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto table = topic_for(kind);
        if (table.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        state_->add_subscription(table, symbol);
        if (client_.connected()) {
            const std::array<Subscription, 1> one{Subscription{.table = table, .symbol = symbol}};
            if (const auto frame = state_->protocol.subscribe_frame(one); frame.has_value()) {
                (void)client_.send(frame->text);
            }
        }
        return {};
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        if (client_.connected() && now.ns >= state_->next_app_ping_ns) {
            (void)client_.send(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitmexData>(std::move(state));
}

} // namespace raikiri::venues::bitmex::detail
