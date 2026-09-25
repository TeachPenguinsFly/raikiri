#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/edgex/book.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>
#include <raikiri/venues/edgex/map.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::edgex::internal {

namespace {

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

[[nodiscard]] int ask_rank(std::string_view channel) noexcept {
    if (channel == k_ticker_all) {
        return 0;
    }
    return channel.starts_with(k_depth_prefix) ? 1 : 2;
}

class EdgexData final : public exec::DataClient {
public:
    explicit EdgexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::stream_host(state_->market)},
                              .port = "443",
                              .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
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

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto contract = state_->contract_of.find(instrument.packed);
        if (contract == state_->contract_of.end()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::string channel;
        switch (kind) {
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            state_->funding_wanted.insert(instrument.packed);
            channel = k_ticker_all;
            break;
        case exec::SubKind::Depth:
            state_->depth_wanted.insert(instrument.packed);
            [[fallthrough]];
        case exec::SubKind::Bbo:
        case exec::SubKind::Trades: {
            std::array<char, 48> buf{};
            channel = channel_name(kind, contract->second, buf);
            break;
        }
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!state_->channels.insert(channel).second) {
            return {};
        }
        const std::array<std::string, 1> one{channel};
        if (!pool_.subscribe(one)) {
            state_->channels.erase(channel);
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "edgex: every socket the pool may open is full")};
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
    }

private:
    class Handler final : public net::WsHandler {
    public:
        explicit Handler(EdgexData& owner) : owner_{&owner} {}

        void on_open() override {
            owner_->state_->sink->on_connectivity(
                {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = owner_->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner_->on_frame(payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner_->state_->sink->on_connectivity(
                {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = owner_->state_->clock->wall()});
        }

    private:
        EdgexData* owner_;
    };

    // a reopened socket broke the version chain; the resubscription's snapshot restores it
    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (const auto book = state_->books.find(depth_contract(key)); book != state_->books.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_event(frame);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Depth:
            on_depth(frame, received);
            break;
        case EventKind::Trades:
            // the snapshot is a backlog of old prints
            if (!event->snapshot) {
                on_trades(frame, received);
            }
            break;
        case EventKind::Ticker:
            on_tickers(frame, received);
            break;
        case EventKind::Ping:
            on_ping(frame);
            break;
        case EventKind::Error:
            on_error(frame);
            break;
        case EventKind::Other:
            break;
        }
    }

    void on_ping(std::string_view frame) noexcept {
        const auto time = parse_ping(frame);
        if (!time.has_value()) {
            return;
        }
        if (const auto pong = state_->protocol.pong_frame(*time); pong.has_value()) {
            (void)pool_.reply(*pong);
        }
    }

    void on_error(std::string_view frame) noexcept {
        const auto err = parse_stream_error(frame);
        if (!err.has_value()) {
            return;
        }
        ++state_->subscribe_refusals;
        state_->last_refusal.assign(err->code);
        state_->last_refusal.append(" on ");
        state_->last_refusal.append(err->channel);
    }

    void on_depth(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_depth(frame, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state_->instrument_of(wire->contract_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state_->books.try_emplace(wire->contract_id, base.id, base.tick_size, base.lot_size).first->second;
        const auto result = book.apply(*wire);
        if (result == BookResult::Gap || result == BookResult::Crossed) {
            resubscribe_book(wire->contract_id);
            return;
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        // no depth frame carries a timestamp
        if (const auto top = book.top(received, received); top.has_value()) {
            state_->sink->on_bbo(*top);
            state_->priced.insert(wire->contract_id);
        }
        if (!state_->depth_wanted.contains(base.id.packed) || !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // reply() reaches only the socket being dispatched, which is the one that
    // carries this book.
    void resubscribe_book(std::uint32_t contract_id) noexcept {
        std::array<char, 48> buf{};
        const auto channel = channel_name(exec::SubKind::Depth, contract_id, buf);
        const auto off = state_->protocol.unsubscribe_frame(channel);
        if (!off.has_value() || !pool_.reply(*off)) {
            return;
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
            const auto* instrument = state_->instrument_of(print.contract_id);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto trade = map_trade(print, base_of(*instrument), received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                state_->traded.insert(print.contract_id);
            }
        }
    }

    void on_tickers(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(frame, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto* instrument = state_->instrument_of(row.contract_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (!state_->funding_wanted.contains(base.id.packed)) {
                continue;
            }
            if (const auto funding = map_funding(row, *instrument, received); funding.has_value()) {
                state_->sink->on_funding(*funding);
                state_->funded.insert(row.contract_id);
            }
            if (const auto ticker = map_ticker_stats(row, base, received); ticker.has_value()) {
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

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::uint64_t gaps = 0;
        std::size_t dark = 0;
        for (const auto& [contract, book] : state_->books) {
            gaps += book.gaps();
            if (!book.synced()) {
                ++dark;
            }
        }
        RLOG_INFO("edgex: {} of {} contracts priced, {} booked ({} unsynced at stop), {} traded, "
                  "{} funded on {} socket(s) — {} broken chain(s), {} resubscribe(s)",
                  state_->priced.size(),
                  state_->by_contract.size(),
                  state_->books.size(),
                  dark,
                  state_->traded.size(),
                  state_->funded.size(),
                  pool_.connection_count(),
                  gaps,
                  state_->resubscribes);
        if (state_->subscribe_refusals != 0) {
            RLOG_WARN("edgex: the venue refused {} subscription(s), the last {}",
                      state_->subscribe_refusals,
                      state_->last_refusal);
        }
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<EdgexData>(std::move(state));
}

} // namespace raikiri::venues::edgex::internal
