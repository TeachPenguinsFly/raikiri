#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/endpoints.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
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

// Routes one public frame. Kept separate from the client so the dispatch is
// testable without a socket.
void handle_public_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto topic = parse_topic(frame);
    if (!topic.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (topic->kind) {
    case TopicKind::Orderbook: {
        static thread_local OrderbookBuffer storage;
        const auto book = parse_orderbook(frame, storage);
        if (!book.has_value()) {
            return;
        }
        const auto* instrument = state.instruments.find(book->symbol);
        if (instrument == nullptr) {
            return;
        }
        auto& sync = state.book_for(*instrument);
        const auto result = sync.apply(*book);
        if (result == SyncResult::GapDetected || result == SyncResult::BadLevel) {
            return;
        }
        const auto ts_event = ms_to_unix(book->ts_ms, received);
        if (const auto bbo = sync.bbo(ts_event, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
        }
        if (const auto depth = sync.snapshot(ts_event, received); depth.has_value()) {
            state.sink->on_depth(*depth);
        }
        break;
    }
    case TopicKind::PublicTrade: {
        static thread_local TradeBuffer storage;
        const auto trades = parse_trades(frame, storage);
        if (!trades.has_value()) {
            return;
        }
        for (const auto& wire : *trades) {
            if (const auto trade = map_trade(wire, state.instruments, received);
                trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case TopicKind::Tickers: {
        const auto ticker = parse_ticker(frame);
        if (!ticker.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*ticker, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*ticker, state.instruments, received);
            stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    default:
        break; // control frames and private topics are not this socket's
    }
}

class PublicHandler final : public net::WsHandler {
public:
    PublicHandler(VenueState& state, MarketType market) : state_{&state}, market_{market} {}

    void on_open() override {
        std::vector<std::string_view> topics;
        topics.reserve(state_->desired_topics.size());
        for (const auto& topic : state_->desired_topics) {
            topics.push_back(topic);
        }
        std::size_t sent = 0;
        while (sent < topics.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{topics}.subspan(sent));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            if (!client_->send(frame->text)) {
                break; // wedged socket; the reconnect will retry the whole set
            }
            sent += frame->consumed;
        }
        // Books cannot survive a gap in the socket, let alone a reconnect.
        for (auto& [id, book] : state_->books) {
            book.invalidate();
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        handle_public_frame(*state_, payload, received);
    }

    void on_close(net::WsCloseReason reason) override {
        state_->sink->on_connectivity({.state = reason == net::WsCloseReason::Requested
                                                    ? exec::ConnState::Disconnected
                                                    : exec::ConnState::Disconnected,
                                       .stream_id = 0,
                                       .ts = state_->clock->wall()});
    }

    void attach(net::WsClient& client) noexcept { client_ = &client; }

    [[nodiscard]] MarketType market() const noexcept { return market_; }

private:
    VenueState* state_;
    MarketType market_;
    net::WsClient* client_{nullptr};
};

// Private order/execution socket.
class BybitData final : public exec::DataClient {
public:
    BybitData(std::shared_ptr<VenueState> state, MarketType market)
        : state_{std::move(state)}, handler_{*state_, market},
          client_{state_->io,
                  net::WsConfig{.host = std::string{host(state_->testnet)},
                                .port = "443",
                                .target = std::string{public_stream_path(market)}},
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
        std::array<char, 64> buf{};
        const auto topic = topic_name(kind, symbol, buf);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        state_->add_topic(topic);
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{topic};
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
            (void)client_.send(state_->protocol.ping_frame());
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

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state,
                                                   MarketType market) {
    return std::make_unique<BybitData>(std::move(state), market);
}

} // namespace raikiri::venues::bybit::detail
