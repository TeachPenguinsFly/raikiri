#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/endpoints.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::deribit::detail {

namespace {

// A pool key is one channel. Deribit names the instrument INSIDE it —
// book.BTC-PERPETUAL.none.1.100ms — so unlike venues that carry the symbol in
// a field beside the channel there is nothing to join, and the pool's opaque
// string is the channel itself.
[[nodiscard]] std::string key_of(std::string_view channel) {
    return std::string{channel};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its
    // own keys when it opens; replaying the whole desired set on every one of
    // them is the flood the pool exists to avoid.
    //
    // AND NO BOOK IS INVALIDATED. Deribit seeds a book from its own `snapshot`
    // on the channel and from nothing else, so a channel asked for again
    // resets itself in Book::apply — while this hook fires per socket and
    // knows none of them apart, so blanking every book here would silence the
    // ones the other sockets are feeding until they happen to reconnect.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        if (handle_frame(*state_, payload, received)) {
            // Answered on the asking socket alone: a broadcast spends the
            // shared request bucket once per socket per socket.
            (void)pool_->reply(state_->protocol.test_frame().text);
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void attach(net::WsPool& pool) noexcept { pool_ = &pool; }

private:
    VenueState* state_;
    net::WsPool* pool_{nullptr};
};

class DeribitData final : public exec::DataClient {
public:
    explicit DeribitData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // Deribit caps a CONNECTION at 1024 channels; the pool turns
                // that into a per-socket number instead of the process's, which
                // is what left 256 of 1812 listings watched.
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {
        handler_.attach(pool_);
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::array<char, k_max_channel_len> buffer{};
        const auto channel = channel_for(kind, symbol, buffer);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired.size();
        state_->add_subscription(channel);
        if (state_->desired.size() == before) {
            return {}; // Funding and MarkPrice name one ticker channel
        }
        // THE POOL PLACES IT. A key that does not fit on any existing socket
        // opens another, and only the pool's own ceiling refuses — which is
        // said out loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{key_of(channel)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "deribit: connection pool is full")};
        }
        // Asked, not accepted: frames.cpp counts the other half off the replies.
        state_->channels_requested = pool_.key_count();
        return {};
    }

    std::size_t wait(std::uint32_t timeout_ms) noexcept override {
        return started_ ? state_->io.run_for(timeout_ms) : 0;
    }

    void poll(UnixNanos now) noexcept override {
        if (!started_) {
            return;
        }
        state_->io.poll();
        pool_.poll(now);
    }

private:
    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;

    // Turns one connection's keys back into the frames that ask for them.
    // Called on every open of that socket, which is why the quote channel is
    // ordered first here rather than at subscribe time: on a reconnect the
    // order that matters is the order the venue is asked in.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            views.push_back({.channel = key});
        }
        // book.<name>.none.1.<interval> is the grouped book one level deep —
        // the Bid/Ask column every screen shows, and the same five segments
        // envelope.cpp reads a Bbo from.
        std::ranges::stable_partition(views, [](const Subscription& one) {
            return one.channel.find(".none.1.") != std::string_view::npos;
        });

        // set_heartbeat is a SESSION property and this builder is the only hook
        // a socket that just opened has. A refill sends it again, which re-arms
        // the same interval and asks for nothing new.
        std::vector<std::string> frames;
        frames.emplace_back(state_->protocol.set_heartbeat_frame(k_heartbeat_interval_s).text);
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(std::span<const Subscription>{views}.subspan(
                built, std::min(net::frame_subscriptions(), views.size() - built)));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<DeribitData>(std::move(state));
}

} // namespace raikiri::venues::deribit::detail
