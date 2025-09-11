#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/venues/bitfinex/map.hpp>
#include <raikiri/venues/bitfinex/rest.hpp>
#include <raikiri/venues/bitfinex/sign.hpp>
#include <raikiri/venues/bitfinex/ws.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::bitfinex::detail {

namespace {

void handle_subscribed(VenueState& state, std::string_view frame) noexcept {
    const auto ack = parse_subscribed(frame);
    if (!ack.has_value()) {
        return;
    }
    const auto kind = channel_kind(ack->channel, ack->key);
    const auto text =
        kind == ChannelKind::DerivStatus ? symbol_from_deriv_key(ack->key) : ack->symbol;
    const auto symbol = SymbolText::from(text);
    if (kind == ChannelKind::Unknown || !symbol.has_value() || symbol->empty()) {
        return;
    }
    state.bind(ack->chan_id, kind, *symbol);
}

void handle_data(VenueState& state,
                 std::string_view frame,
                 const FrameInfo& info,
                 UnixNanos received) noexcept {
    const auto* binding = state.binding(info.chan_id);
    if (binding == nullptr) {
        ++state.unbound_frames;
        return;
    }
    const auto ts_event = ms_to_unix(info.ts_ms, UnixNanos{0});
    switch (binding->kind) {
    case ChannelKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo =
                map_bbo(*wire, binding->symbol.view(), state.instruments, ts_event, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto stats = map_ticker_stats(
                *wire, binding->symbol.view(), state.instruments, ts_event, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case ChannelKind::Trades: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        if (wire->kind != TradeFrameKind::Executed) {
            return;
        }
        for (const auto& entry : wire->trades) {
            if (const auto trade =
                    map_trade(entry, binding->symbol.view(), state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case ChannelKind::DerivStatus: {
        const auto wire = parse_deriv_status(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding =
                map_funding(*wire, binding->symbol.view(), state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case ChannelKind::Book: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_book(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(binding->symbol.view());
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state.book_for(base);
        const auto result = book.apply(*wire);
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return; // unsynced or dropped: silence, never a guess
        }
        // A book frame carries no timestamp of any kind, so ingest is the only
        // honest ts_event this channel can offer.
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        // The fast top of book, beside the ticker's rather than instead of it.
        if (const auto bbo = book.touch(received, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case ChannelKind::Unknown:
        break;
    }
}

// noexcept, and so are the two above, because the registry allocates: binding
// a channel id copies a symbol, and `channels[id]` grows a map. This is the
// only adapter here whose inbound path can allocate at all. An escape from an
// asio callback holding live market data is not something to unwind through,
// and the codebase already aborts rather than continues on a corrupt number
// (fixed_point.hpp), so failing loudly here is the consistent answer.
void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) noexcept {
    const auto info = parse_frame(frame);
    if (!info.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (info->kind) {
    case FrameKind::Data:
        handle_data(state, frame, *info, received);
        break;
    case FrameKind::Subscribed:
        handle_subscribed(state, frame);
        break;
    case FrameKind::Info:
        if (info->code == k_info_reconnect || info->code == k_info_maintenance_end) {
            state.rebind_from_scratch();
        }
        break;
    case FrameKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case FrameKind::Auth:
        // The public socket never authenticates; the execution client's does,
        // and it reads this frame on its own handler.
        break;
    case FrameKind::Heartbeat:
    case FrameKind::Unsubscribed:
    case FrameKind::Pong:
    case FrameKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->rebind_from_scratch();
        (void)client_->send(Protocol::conf_frame());
        for (const auto& entry : state_->subscriptions()) {
            const auto frame = state_->protocol.subscribe_frame(
                {.kind = entry.kind, .symbol = entry.symbol.view()});
            if (!frame.has_value()) {
                continue;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason) override {
        state_->rebind_from_scratch();
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BitfinexData final : public exec::DataClient {
public:
    explicit BitfinexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host()},
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
        const auto channel = kind_for(kind);
        if (channel == ChannelKind::Unknown) {
            return DataClient::subscribe(kind, instrument);
        }
        if (channel == ChannelKind::DerivStatus && instrument.market() != MarketType::Perp) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto owned = SymbolText::from(symbol);
        if (!owned.has_value()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "symbol too long")};
        }
        if (!state_->add_subscription(channel, *owned)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        if (client_.connected()) {
            if (const auto frame =
                    state_->protocol.subscribe_frame({.kind = channel, .symbol = symbol});
                frame.has_value()) {
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
        // The execution client's authenticated socket rides this timer too:
        // ExecutionClient has no poll hook, and a socket with no public
        // subscriptions receives no heartbeat to answer.
        if (state_->private_poll) {
            state_->private_poll(now);
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
    return std::make_unique<BitfinexData>(std::move(state));
}

} // namespace raikiri::venues::bitfinex::detail
