#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/ticker_poll.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bitget/book.hpp>
#include <raikiri/venues/bitget/endpoints.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bitget::internal {

namespace {

// bitget drops a socket idle for 30 s and ignores control frames, so the app ping runs inside that
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;

// mark, index and the rate move by the second; the 24h window on spot needs far less
constexpr std::uint64_t k_perp_tickers_interval_ns = 2'000'000'000ULL;
constexpr std::uint64_t k_spot_tickers_interval_ns = 15'000'000'000ULL;

// books1 pushes only on a change, so a tickers row restates a top this long unmoved
constexpr std::uint64_t k_quiet_quote_ms = 30'000;

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // unknown shapes, including the bare pong, are ignored
        return;
    }
    switch (event->kind) {
    case EventKind::Top: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            // a tickers row may already have restated a later top
            auto& last = state.quoted_ms[bbo->instrument];
            if (wire->ts_ms >= last) {
                last = wire->ts_ms;
                state.sink->on_bbo(*bbo);
            }
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
            if (const auto trade = map_trade(entry, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
            }
        }
        break;
    }
    case EventKind::TradeSnapshot:
        break; // the last 50 prints, minutes old on a thin contract: history, not trades
    case EventKind::Books: {
        static thread_local LevelBuffer levels;
        const auto wire = parse_books(frame, levels);
        if (!wire.has_value()) {
            return;
        }
        if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
            state.sink->on_depth(*book);
        }
        break;
    }
    case EventKind::Error: {
        // terminal for the channel it names
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::Orders:
    case EventKind::LoginAck:
        break; // the private socket's, and it never shares this handler
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break;
    }
}

// a pool key is channel and contract joined by a byte neither contains
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view inst_id) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(inst_id);
    return key;
}

// the only decode of key_of; returns views into the pool-owned key
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key, std::string_view type) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.inst_type = type, .channel = key.substr(0, cut), .inst_id = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each socket resubscribes its own keys on open; books1 and books15 frames are full snapshots, so no reseed
    void on_open() override {
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

private:
    VenueState* state_;
};

class BitgetData final : public exec::DataClient {
public:
    explicit BitgetData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host()},
                              .port = std::string{stream_port()},
                              .target = std::string{stream_path()}},
                handler_,
                // the ceiling is per connection
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::rest_host()}, .port = "443"}},
          tickers_{state_->market == MarketType::Spot ? k_spot_tickers_interval_ns : k_perp_tickers_interval_ns} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const bool funding = kind == exec::SubKind::Funding || kind == exec::SubKind::MarkPrice;
        if (funding && state_->market != MarketType::Spot) {
            // the rate, mark and index come with the tickers list, the settlement clock from current-fund-rate
            tickers_.want(instrument);
            fund_rates_.want(instrument);
            return {};
        }
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!state_->add_subscription(channel, symbol)) {
            return {};
        }
        // the pool places it, opening another socket if none fits
        const std::array<std::string, 1> one{key_of(channel, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitget: connection pool is full")};
        }
        if (kind == exec::SubKind::Bbo) {
            // the 24h window rides the tickers list too; a refused quote must not be priced from it
            tickers_.want(instrument);
            state_->quoted_ms.try_emplace(instrument, 0);
        }
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
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            // every socket: the idle timer is per connection
            pool_.broadcast(Protocol::ping_frame());
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        tickers_.poll(now, [this] {
            return rest_.send(builder_.tickers(state_->market, state_->product()),
                              [this](const net::RestResponse& response) { on_tickers(response); });
        });
        fund_rates_.poll(now, [this] {
            return rest_.send(builder_.current_fund_rates(state_->product()),
                              [this](const net::RestResponse& response) { on_fund_rates(response); });
        });
    }

private:
    void on_tickers(const net::RestResponse& response) noexcept {
        tickers_.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.inst_id);
            if (instrument == nullptr || !tickers_.wanted(base_of(*instrument).id)) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            if (const auto stats = map_ticker_stats(row, state_->instruments, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
            if (const auto clock = state_->funding_clocks.find(id); clock != state_->funding_clocks.end()) {
                if (const auto funding = map_funding(row, clock->second, state_->instruments, received);
                    funding.has_value()) {
                    state_->sink->on_funding(*funding);
                }
            }
            // a later row confirms a top books1 has had no reason to push again
            const auto quoted = state_->quoted_ms.find(id);
            if (quoted != state_->quoted_ms.end() && row.ts_ms >= quoted->second + k_quiet_quote_ms) {
                if (const auto bbo = map_bbo(row, state_->instruments, received); bbo.has_value()) {
                    quoted->second = row.ts_ms;
                    state_->sink->on_bbo(*bbo);
                }
            }
        }
    }

    void on_fund_rates(const net::RestResponse& response) noexcept {
        fund_rates_.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<FundRateBuffer>();
        const auto rows = parse_current_fund_rates(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr || !fund_rates_.wanted(base_of(*instrument).id)) {
                continue;
            }
            if (const auto clock = funding_clock_of(row); clock.has_value()) {
                state_->funding_clocks[base_of(*instrument).id] = *clock;
            }
        }
    }

    // runs on every open of the socket, so this order is the order the venue is asked in
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        const auto type = inst_type(state_->market, state_->testnet);
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key, type); one.has_value()) {
                views.push_back(*one);
            }
        }
        // the quote first; with paced sends the last frames may lag
        const auto quote = channel_for(exec::SubKind::Bbo);
        std::ranges::stable_partition(views, [quote](const Subscription& one) { return one.channel == quote; });

        std::vector<std::string> frames;
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

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    RequestBuilder builder_;
    exec::TickerPoll tickers_;
    exec::TickerPoll fund_rates_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitgetData>(std::move(state));
}

} // namespace raikiri::venues::bitget::internal
