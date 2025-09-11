#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/mexc/errors.hpp>
#include <raikiri/venues/mexc/map.hpp>
#include <raikiri/venues/mexc/rest.hpp>
#include <raikiri/venues/mexc/spot_wire.hpp>
#include <raikiri/venues/mexc/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "clients.hpp"

namespace raikiri::venues::mexc::detail {

namespace {

// MEXC closes a socket that has sent nothing for 60 s and counts only CLIENT
// messages toward that timer — verified: a connection receiving a push every
// three seconds was still dropped at 62 s. Ten seconds leaves room for a
// missed ping and a reconnect inside the window.
constexpr std::uint64_t k_app_ping_interval_ns = 10'000'000'000ULL;

// One spot frame. Protobuf, so nothing here is text and the JSON classifier
// never sees it: what the frame is is decided by WHICH payload field number is
// present, and an unknown one is skipped rather than guessed at.
//
// The subscription acks are JSON on the same socket. They start with '{' and
// no protobuf message can, so they cost one byte to tell apart.
void handle_spot_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    if (frame.empty() || frame.front() == '{') {
        return; // a control frame; the subscribe path already reports refusals
    }
    const auto event = parse_stream_event(frame);
    if (!event.has_value()) {
        return;
    }
    switch (event->kind) {
    case StreamKind::Quote: {
        const auto quote = parse_stream_quote(event->payload);
        if (!quote.has_value()) {
            return;
        }
        if (const auto bbo =
                map_spot_bbo(*quote, event->symbol, event->ts_ms, state.instruments, received);
            bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case StreamKind::Trade: {
        static thread_local StreamTradeBuffer trades;
        const auto parsed = parse_stream_trades(event->payload, trades);
        if (!parsed.has_value()) {
            return;
        }
        for (const auto& entry : *parsed) {
            if (const auto trade =
                    map_spot_trade(entry, event->symbol, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case StreamKind::Depth: {
        static thread_local LevelBuffer levels;
        const auto book = parse_stream_book(event->payload, levels);
        if (!book.has_value()) {
            return;
        }
        if (const auto depth =
                map_spot_book(*book, event->symbol, event->ts_ms, state.instruments, received);
            depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case StreamKind::Unknown:
        break;
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal
    }
    switch (event->kind) {
    case EventKind::DepthFull: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_depth(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Trade: {
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& entry : *wire) {
            if (const auto trade = map_trade(entry, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::Ticker: {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::Depth:
        break;
    case EventKind::Error: {
        if (parse_stream_error(frame).has_value()) {
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::PersonalOrder:
    case EventKind::PersonalDeal:
    case EventKind::PersonalPosition:
    case EventKind::LoginAck:
        break; // private kinds belong to the execution client's own socket
    case EventKind::SubscribeAck:
    case EventKind::Pong:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (const auto& entry : state_->desired) {
            const std::array<Subscription, 1> one{Subscription{
                .channel = entry.channel, .symbol = entry.symbol, .limit = entry.limit}};
            const auto frame = state_->protocol.subscribe_frame(one);
            if (!frame.has_value() || !client_->send(frame->text)) {
                break; // wedged socket; the reconnect retries the whole set
            }
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        if (state_->market == MarketType::Spot) {
            handle_spot_frame(*state_, payload, received);
            return;
        }
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

class MexcData final : public exec::DataClient {
public:
    explicit MexcData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->market)},
                                .port = std::string{stream_port()},
                                .target = std::string{stream_path(state_->market)}},
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
        if (instrument.market() != state_->market) {
            // One socket, one tree, and the trees do not even share an
            // encoding — a frame from the other one would not parse here.
            return std::unexpected{exec::not_implemented("mexc: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const bool spot = state_->market == MarketType::Spot;
        const auto channel = spot ? spot_channel_for(kind) : channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->desired.size() >= k_max_subscriptions_per_connection) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "subscription limit reached")};
        }
        // The level count rides the channel name on spot and the param object
        // on futures, and only the depth channel takes one on spot.
        const std::uint32_t limit = spot ? (kind == exec::SubKind::Depth ? k_spot_book_depth : 0)
                                         : (kind == exec::SubKind::Bbo ? k_book_depth : 0);
        state_->add_subscription(channel, symbol, limit);
        if (client_.connected()) {
            const Subscription one{.channel = channel, .symbol = symbol, .limit = limit};
            const auto frame = spot ? state_->protocol.spot_subscribe_frame(one)
                                    : state_->protocol.subscribe_frame(std::span{&one, 1});
            if (frame.has_value()) {
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
        if (now.ns < state_->next_app_ping_ns) {
            return;
        }
        if (client_.connected()) {
            (void)client_.send(Protocol::ping_frame());
        }
        if (state_->private_client != nullptr && state_->private_client->connected()) {
            (void)state_->private_client->send(Protocol::ping_frame());
        }
        state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<MexcData>(std::move(state));
}

} // namespace raikiri::venues::mexc::detail
