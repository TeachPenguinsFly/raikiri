#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/venues/backpack/endpoints.hpp>
#include <raikiri/venues/backpack/envelope.hpp>
#include <raikiri/venues/backpack/map.hpp>
#include <raikiri/venues/backpack/market_data.hpp>
#include <raikiri/venues/backpack/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::backpack::internal {

namespace {

// 20 a second, under the venue's 2000 a minute
constexpr std::uint64_t k_seed_spacing_ns = 50ULL * 1'000'000ULL;

// five is the smallest limit the venue takes
constexpr std::uint32_t k_touch_depth = 5;

constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

constexpr std::uint64_t k_book_report_interval_ns = 15ULL * 1'000'000'000ULL;

// the venue sends no limit header, so a 429 is the only correction
void heed_rate_refusal(VenueState& state, const net::RestResponse& response) noexcept {
    if (state.budget != nullptr && response.status == 429) {
        state.budget->penalize_all(state.clock->mono(), 1'000'000'000ULL);
    }
}

[[nodiscard]] bool wants_funding(const VenueState& state, InstrumentId id) noexcept {
    return std::ranges::find(state.funding_instruments, id) != state.funding_instruments.end();
}

// a quiet pair sends no delta that would ask again
void requeue(std::vector<InstrumentId>& queue, InstrumentId id) {
    if (std::ranges::find(queue, id) == queue.end()) {
        queue.push_back(id);
    }
}

[[nodiscard]] bool request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        static thread_local LevelBuffer storage;
        const bool answered = !response.transport_failed() && response.status == 200;
        const auto wire = answered ? parse_depth_snapshot(response.body, storage)
                                   : std::unexpected(WireError::NotJson);
        if (!wire.has_value()) {
            heed_rate_refusal(*owner, response);
            requeue(owner->pending_seeds, id); // still marked in flight so the delta path stays out
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        const auto ts_event = us_to_unix(wire->timestamp_us, response.received);
        if (const auto seeded = target.snapshot(ts_event, response.received); seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    const auto request = state.builder.depth_snapshot(base.exchange_symbol.view(), k_seed_depth);
    if (request.target.empty() || !state.rest.send(request, handled)) {
        book.seed_failed();
        return false;
    }
    book.seed_requested();
    return true;
}

[[nodiscard]] bool request_touch(VenueState& state, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        if (owner->quoted.contains(id.packed)) {
            return; // the stream is newer
        }
        if (response.transport_failed() || response.status != 200) {
            heed_rate_refusal(*owner, response);
            requeue(owner->pending_touches, id);
            return;
        }
        const auto* listed = owner->instruments.find(id);
        if (listed == nullptr) {
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_depth_snapshot(response.body, storage);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_touch(*wire, base_of(*listed), response.received);
            bbo.has_value()) {
            owner->sink->on_bbo(*bbo);
        }
    };
    const auto request = state.builder.depth_snapshot(base.exchange_symbol.view(), k_touch_depth);
    return !request.target.empty() && state.rest.send(request, handled);
}

void handle_depth(VenueState& state, std::string_view data, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_depth_update(data, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->symbol);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    auto& book = state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
                     .first->second;
    // asked before the delta is offered so the triggering frame buffers behind it
    if (book.wants_seed()) {
        if (std::ranges::find(state.pending_seeds, base.id) == state.pending_seeds.end()) {
            state.pending_seeds.push_back(base.id);
        }
        book.seed_requested();
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return;
    }
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(us_to_unix(wire->engine_us, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    switch (parse_event_kind(*data)) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
            state.quoted.insert(bbo->instrument.packed);
            state.sink->on_bbo(*bbo);
        }
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
        }
        break;
    }
    case EventKind::MarkPrice: {
        const auto wire = parse_mark_price(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::Depth:
        handle_depth(state, *data, received);
        break;
    case EventKind::Error:
        ++state.subscribe_refusals;
        break;
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

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

class BackpackData final : public exec::DataClient {
public:
    explicit BackpackData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { invalidate_books(keys); }} {
        meter_market_data(*state_, state_->rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->rest.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        state_->rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const bool perp = instrument.market() == MarketType::Perp;
        if (kind == exec::SubKind::Funding) {
            if (!wants_funding(*state_, instrument)) {
                state_->funding_instruments.push_back(instrument);
            }
            if (!perp) {
                return {};
            }
            kind = exec::SubKind::MarkPrice;
        }
        if (kind == exec::SubKind::MarkPrice && !perp) {
            return DataClient::subscribe(kind, instrument);
        }
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (!desired_.emplace(stream).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::string{stream}};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "backpack: connection pool is full")};
        }
        if (kind == exec::SubKind::Bbo) {
            state_->pending_touches.push_back(instrument);
        }
        // a pair whose book does not move sends no delta to trigger a seed
        if (kind == exec::SubKind::Depth) {
            const auto* listed = state_->instruments.find(instrument);
            const auto& base = base_of(*listed);
            auto& book =
                state_->books
                    .try_emplace(instrument.packed, instrument, base.tick_size, base.lot_size)
                    .first->second;
            if (book.wants_seed()) {
                state_->pending_seeds.push_back(instrument);
                book.seed_requested();
            }
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
        drain_seeds(now);
        poll_tickers(now);
        report_books(now);
    }

private:
    // an open breaks the chain of this socket's books; each asks for its own seed
    void invalidate_books(std::span<const std::string> keys) noexcept {
        for (const auto& key : keys) {
            const auto symbol = depth_symbol(key);
            if (symbol.empty()) {
                continue;
            }
            const auto* instrument = state_->instruments.find(symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            const auto entry = state_->books.find(id.packed);
            if (entry == state_->books.end()) {
                continue;
            }
            entry->second.invalidate();
            requeue(state_->pending_seeds, id);
            entry->second.seed_requested();
        }
    }

    // quote first: with paced sends a short run never sees the last stream
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> streams(keys.begin(), keys.end());
        std::ranges::stable_partition(
            streams, [](std::string_view stream) { return stream.starts_with(k_bbo_stream); });
        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < streams.size()) {
            const auto frame =
                state_->protocol.subscribe_frame(std::span<const std::string_view>{streams}.subspan(
                    built, std::min(net::frame_subscriptions(), streams.size() - built)));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    // book seeds go before touches since a delta already waits on them
    void drain_seeds(UnixNanos now) noexcept {
        if (now.ns < state_->next_seed_ns) {
            return;
        }
        if (state_->pending_seeds.empty()) {
            drain_touch(now);
            return;
        }
        const auto id = state_->pending_seeds.front();
        const auto entry = state_->books.find(id.packed);
        const auto* listed = state_->instruments.find(id);
        if (entry == state_->books.end() || listed == nullptr) {
            state_->pending_seeds.erase(state_->pending_seeds.begin());
            return;
        }
        if (!request_seed(*state_, entry->second, base_of(*listed))) {
            return;
        }
        state_->pending_seeds.erase(state_->pending_seeds.begin());
        state_->next_seed_ns = now.ns + k_seed_spacing_ns;
    }

    void drain_touch(UnixNanos now) noexcept {
        auto& queue = state_->pending_touches;
        while (!queue.empty() && state_->quoted.contains(queue.front().packed)) {
            queue.erase(queue.begin()); // the stream already spoke; nothing to cover
        }
        if (queue.empty()) {
            return;
        }
        const auto* listed = state_->instruments.find(queue.front());
        if (listed != nullptr && !request_touch(*state_, base_of(*listed))) {
            return;
        }
        queue.erase(queue.begin());
        state_->next_seed_ns = now.ns + k_seed_spacing_ns;
    }

    void poll_tickers(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < next_ticker_poll_ns_ ||
            ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        try {
            if (!state_->rest.send(
                    state_->builder.tickers(),
                    [this](const net::RestResponse& response) { on_tickers(response); })) {
                ticker_in_flight_ = false;
                return;
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        next_ticker_poll_ns_ = now.ns + k_ticker_poll_interval_ns;
    }

    void on_tickers(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            heed_rate_refusal(*state_, response);
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto stats = map_ticker_stats(row, state_->instruments, response.received);
            if (stats.has_value() && wants_funding(*state_, stats->instrument)) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    // reported only after two looks so a brief resync is not flagged
    void report_books(UnixNanos now) noexcept {
        if (now.ns < report_due_ns_ || state_->books.empty() || !has_logger()) {
            return;
        }
        report_due_ns_ = now.ns + k_book_report_interval_ns;
        const auto cold = cold_books();
        const bool behind = cold != 0 && (!state_->pending_seeds.empty() || cold_before_ != 0);
        cold_before_ = cold;
        if (!behind) {
            if (warned_) {
                warned_ = false;
                RLOG_INFO("backpack: every one of {} depth books is seeded", state_->books.size());
            }
            return;
        }
        warned_ = true;
        RLOG_WARN("backpack: {} of {} depth books have no seed yet, {} still queued",
                  cold,
                  state_->books.size(),
                  state_->pending_seeds.size());
    }

    void report_final() const noexcept {
        if (state_->books.empty() || !has_logger()) {
            return;
        }
        std::uint64_t gaps = 0;
        std::uint64_t reseeds = 0;
        for (const auto& [packed, book] : state_->books) {
            gaps += book.gaps();
            reseeds += book.reseeds();
        }
        const auto cold = cold_books();
        if (cold == 0) {
            RLOG_INFO("backpack: {} depth books, all seeded — {} chain gap(s), {} reseed(s)",
                      state_->books.size(),
                      gaps,
                      reseeds);
            return;
        }
        std::string names;
        std::size_t named = 0;
        for (const auto& [packed, book] : state_->books) {
            if (book.live() || named == 5) {
                continue;
            }
            names.append(named++ == 0 ? "" : ", ");
            names.append(state_->instruments.exchange_symbol(InstrumentId{packed}));
        }
        RLOG_WARN("backpack: {} of {} depth books NEVER SEEDED ({}) — {} chain gap(s), {} "
                  "reseed(s)",
                  cold,
                  state_->books.size(),
                  names,
                  gaps,
                  reseeds);
    }

    [[nodiscard]] std::size_t cold_books() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            state_->books, [](const auto& entry) { return !entry.second.live(); }));
    }

    [[nodiscard]] static bool has_logger() noexcept { return ::raikiri::log::logger() != nullptr; }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    std::unordered_set<std::string> desired_;
    bool started_{false};
    bool ticker_in_flight_{false};
    bool warned_{false};
    std::uint64_t next_ticker_poll_ns_{0};
    std::uint64_t report_due_ns_{0};
    std::size_t cold_before_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BackpackData>(std::move(state));
}

} // namespace raikiri::venues::backpack::internal
