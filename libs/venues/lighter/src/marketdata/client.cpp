#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/lighter/book.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/venues/lighter/errors.hpp>
#include <raikiri/venues/lighter/map.hpp>
#include <raikiri/venues/lighter/market_data.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::lighter::detail {

namespace {

// While the ramp is still asking. A full perpetual catalogue is ~420
// subscriptions against 200 messages a minute, so a gateway spends over two
// minutes subscribing — and a board half-full for that long reads as a broken
// venue unless something says why.
constexpr std::uint64_t k_ramp_report_interval_ns = 30ULL * 1'000'000'000ULL;

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

// Order in which a socket asks for its keys: the stats stream first, because
// it is ONE message for every perp's funding and mark; then the books, which
// are the quote; trades last. Past the first minute whatever is late waits for
// the venue's allowance, so this order decides what the board has first.
[[nodiscard]] int ask_rank(std::string_view channel) noexcept {
    if (channel == k_perp_stats_all || channel == k_spot_stats_all) {
        return 0;
    }
    if (channel.starts_with(k_book_channel)) {
        return 1;
    }
    return 2;
}

class LighterData final : public exec::DataClient {
public:
    explicit LighterData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{stream_path()}},
                handler_,
                net::WsPoolConfig{.max_keys_per_connection = k_max_subscriptions_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { on_socket_open(keys); }} {}

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto market = state_->market_of.find(instrument.packed);
        if (market == state_->market_of.end()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::string channel;
        switch (kind) {
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            // A spot row has no funding, and its stats stream is asked for
            // the 24-hour window alone.
            state_->funding_wanted.insert(instrument.packed);
            channel = instrument.market() == MarketType::Spot ? k_spot_stats_all : k_perp_stats_all;
            break;
        case exec::SubKind::Depth:
            state_->depth_wanted.insert(instrument.packed);
            [[fallthrough]];
        case exec::SubKind::Bbo:
        case exec::SubKind::Trades: {
            std::array<char, 48> buf{};
            channel = channel_name(kind, market->second, buf);
            break;
        }
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!state_->channels.insert(channel).second) {
            return {}; // Bbo and Depth share a book; every perp shares one stats key
        }
        const std::array<std::string, 1> one{channel};
        if (!pool_.subscribe(one)) {
            state_->channels.erase(channel);
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal,
                0,
                "lighter: the one socket this IP's message budget allows is full")};
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
        report_ramp(now);
    }

private:
    class Handler final : public net::WsHandler {
    public:
        explicit Handler(LighterData& owner) : owner_{&owner} {}

        // Nothing is resubscribed or invalidated here: the pool re-asks for
        // this socket's keys, and on_socket_open drops the books they fed.
        void on_open() override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner_->on_frame(payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

    private:
        LighterData* owner_;
    };

    // A reopened socket's books lost their chain at the close; the snapshot
    // each resubscription brings is the only way back, and until it lands they
    // publish nothing.
    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (!key.starts_with(k_book_channel)) {
                continue;
            }
            const auto cut = key.rfind('/');
            std::uint32_t market_id = 0;
            const auto digits = std::string_view{key}.substr(cut + 1);
            if (std::from_chars(digits.data(), digits.data() + digits.size(), market_id).ec !=
                std::errc{}) {
                continue;
            }
            if (const auto book = state_->books.find(market_id); book != state_->books.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_event(frame);
        if (!event.has_value()) {
            return; // unknown shapes are ignored, never fatal: venues add frames
        }
        switch (event->kind) {
        case EventKind::OrderBook:
            on_book(frame, received);
            break;
        case EventKind::Trade:
            // The subscribe frame replays the last fifty prints, some of them
            // hours old; the tape starts at the first update.
            if (!event->snapshot) {
                on_trades(frame, received);
            }
            break;
        case EventKind::MarketStats:
        case EventKind::SpotMarketStats:
            on_stats(frame, received);
            break;
        case EventKind::Error:
            if (const auto err = parse_stream_error(frame); err.has_value()) {
                ++state_->subscribe_refusals;
                state_->last_error_code = err->code;
            }
            break;
        case EventKind::Connected:
        case EventKind::Unknown:
            break;
        }
    }

    void on_book(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_order_book(frame, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state_->instrument_of(wire->market_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book =
            state_->books.try_emplace(wire->market_id, base.id, base.tick_size, base.lot_size)
                .first->second;
        const auto result = book.apply(*wire);
        if (result == BookResult::Gap) {
            resubscribe_book(wire->market_id);
            return;
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return; // unsynced or off the grid: silence, never a guess
        }
        const UnixNanos ts_event{wire->updated_us != 0 ? wire->updated_us * 1'000ULL : received.ns};
        if (const auto top = book.top(ts_event, received); top.has_value()) {
            state_->sink->on_bbo(*top);
        }
        if (!state_->depth_wanted.contains(base.id.packed) ||
            !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // Asked on the socket that carried the broken chain, which is the one
    // being dispatched: reply() cannot reach any other. Two messages out of the
    // IP's two hundred a minute, and only once per gap — the book answers
    // Ignored, not Gap, until the new snapshot lands.
    void resubscribe_book(std::uint32_t market_id) noexcept {
        std::array<char, 48> buf{};
        const auto channel = channel_name(exec::SubKind::Depth, market_id, buf);
        const auto off = state_->protocol.unsubscribe_frame(channel);
        if (!off.has_value() || !pool_.reply(*off)) {
            return; // the reconnect resubscribes it instead
        }
        const auto on = state_->protocol.subscribe_frame(channel);
        if (on.has_value() && pool_.reply(*on)) {
            ++state_->resubscribes;
        }
    }

    void on_trades(std::string_view frame, UnixNanos received) noexcept {
        static thread_local PrintBuffer storage;
        const auto prints = parse_trades(frame, storage);
        if (!prints.has_value()) {
            return;
        }
        for (const auto& print : *prints) {
            const auto* instrument = state_->instrument_of(print.market_id);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto trade = map_trade(print, base_of(*instrument), received);
                trade.has_value()) {
                state_->sink->on_trade(*trade);
                state_->traded.insert(print.market_id);
            }
        }
    }

    // One `all` frame carries every market that changed; only the ones the
    // gateway asked for reach the sink.
    void on_stats(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<StatsBuffer>();
        const auto stats = parse_market_stats(frame, *storage);
        if (!stats.has_value()) {
            return;
        }
        for (const auto& row : stats->rows) {
            const auto* instrument = state_->instrument_of(row.market_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (!state_->funding_wanted.contains(base.id.packed)) {
                continue;
            }
            if (const auto funding = map_funding(row, base, stats->ts_ms, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                state_->funded.insert(row.market_id);
            }
            if (const auto ticker = map_ticker_stats(row, base, stats->ts_ms, received);
                ticker.has_value()) {
                state_->sink->on_ticker_stats(*ticker);
            }
        }
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> channels{keys.begin(), keys.end()};
        std::ranges::stable_sort(channels, {}, ask_rank);
        std::vector<std::string> frames;
        frames.reserve(channels.size());
        for (const auto channel : channels) {
            if (const auto frame = state_->protocol.subscribe_frame(channel); frame.has_value()) {
                frames.emplace_back(*frame);
            }
        }
        return frames;
    }

    void report_ramp(UnixNanos now) noexcept {
        if (now.ns < ramp_due_ns_ || !has_logger()) {
            return;
        }
        ramp_due_ns_ = now.ns + k_ramp_report_interval_ns;
        const auto queued = pool_.pending_sends();
        if (queued == 0) {
            if (ramping_) {
                ramping_ = false;
                RLOG_INFO("lighter: all {} subscriptions sent", pool_.key_count());
            }
            return;
        }
        ramping_ = true;
        RLOG_INFO("lighter: {} of {} subscriptions still queued — the venue takes 200 "
                  "messages a minute per IP, so the rest of the catalogue is not priced yet",
                  queued,
                  pool_.key_count());
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::uint64_t gaps = 0;
        std::size_t dark = 0;
        for (const auto& [market, book] : state_->books) {
            gaps += book.gaps();
            if (!book.synced()) {
                ++dark;
            }
        }
        RLOG_INFO("lighter: {} of {} markets booked ({} unsynced at stop), {} traded, {} funded "
                  "— {} chain gap(s), {} resubscribe(s)",
                  state_->books.size(),
                  state_->by_market.size(),
                  dark,
                  state_->traded.size(),
                  state_->funded.size(),
                  gaps,
                  state_->resubscribes);
        if (state_->subscribe_refusals != 0) {
            RLOG_WARN("lighter: the venue refused {} message(s), the last with code {}",
                      state_->subscribe_refusals,
                      state_->last_error_code);
        }
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    bool started_{false};
    bool ramping_{false};
    std::uint64_t ramp_due_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<LighterData>(std::move(state));
}

} // namespace raikiri::venues::lighter::detail
