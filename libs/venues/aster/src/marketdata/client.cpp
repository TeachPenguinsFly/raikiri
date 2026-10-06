#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/market_data.hpp>
#include <raikiri/venues/aster/rest.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::aster::internal {

namespace {

// the funding rate changes slowly; poll once a minute
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

// spreads seeds out; the admit gate is the real limit
constexpr std::uint64_t k_seed_spacing_ns = 25ULL * 1'000'000ULL;

constexpr std::uint64_t k_book_report_interval_ns = 15ULL * 1'000'000'000ULL;

// the 24h table costs weight 40; poll every five minutes
constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

// a 429 or 418 drains the shared bucket for a second
void heed_rate_refusal(VenueState& state, const net::RestResponse& response) noexcept {
    if (state.budget == nullptr) {
        return;
    }
    const auto err = parse_rest_error(response.body);
    const bool refused = response.status == 429 || response.status == 418 ||
                         (err.has_value() && classify(err->code) == exec::ErrorClass::RateLimited);
    if (!refused) {
        return;
    }
    state.budget->penalize_all(state.clock->mono(), 1'000'000'000ULL);
}

// a send refused by the pool or budget is retried on the next delta
[[nodiscard]] bool request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed()) {
            target.seed_failed();
            return;
        }
        if (const auto err = parse_rest_error(response.body); err.has_value() && err->code != k_ok) {
            heed_rate_refusal(*owner, response);
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_depth_snapshot(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        const auto ts_event = ms_to_unix(wire->transaction_ms, UnixNanos{});
        if (const auto seeded = target.snapshot(ts_event, response.received); seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    if (!state.depth_rest.send(state.builder.depth_snapshot(base.exchange_symbol.view(), k_seed_depth), handled)) {
        // undo the in-flight mark so the book can ask again
        book.seed_failed();
        return false;
    }
    book.seed_requested();
    return true;
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
    const auto entry = state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // checked before offering so the triggering frame is buffered behind the seed
    if (book.wants_seed()) {
        // queued; poll() paces the seeds
        if (std::ranges::find(state.pending_seeds, base.id) == state.pending_seeds.end()) {
            state.pending_seeds.push_back(base.id);
        }
        book.seed_requested();
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    // skip building the snapshot when the sink does not want it
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->event_ms, UnixNanos{}), received); snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    // unwrap once and read the event from the payload
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    const auto event = parse_event_payload(*data);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, state.instruments, received); bbo.has_value()) {
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
        if (const auto funding = map_funding(*wire, state.instruments, received); funding.has_value()) {
            state.sink->on_funding(*funding);
        }
        break;
    }
    case EventKind::DepthUpdate:
        handle_depth(state, *data, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::SubscribeAck:
    case EventKind::OrderUpdate:
    case EventKind::AccountUpdate:
    case EventKind::ListenKeyExpired:
    case EventKind::Unknown:
        break;
    }
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each socket resubscribes its own streams on open
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

class AsterData final : public exec::DataClient {
public:
    explicit AsterData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet, state_->market)},
                              .port = "443",
                              .target = std::string{stream_path()}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {
        // seeds, funding and ticker polls share the order path's weight
        meter_market_data(*state_, state_->depth_rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (state_->market == MarketType::Spot && (kind == exec::SubKind::Bbo || kind == exec::SubKind::Funding)) {
            state_->spot_tickers.want(instrument);
            if (kind == exec::SubKind::Funding) {
                return {}; // spot pays no funding; the poll is its 24h window
            }
        }
        if (kind == exec::SubKind::Funding) {
            for (const auto existing : state_->funding_instruments) {
                if (existing == instrument) {
                    return {};
                }
            }
            state_->funding_instruments.push_back(instrument);
            return {};
        }
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const auto before = state_->desired_streams.size();
        state_->add_stream(stream);
        if (state_->desired_streams.size() == before) {
            return {}; // already placed on a socket; a stream is asked for once
        }
        // the pool opens another socket when none has room
        const std::array<std::string, 1> one{std::string{stream}};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "aster: connection pool is full")};
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
        report_books(now);
        poll_funding(now);
        poll_ticker(now);
        state_->spot_tickers.poll(now, [this] {
            return state_->depth_rest.send(state_->builder.ticker_24h(),
                                           [this](const net::RestResponse& response) { on_spot_ticker(response); });
        });
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    // logs cold books so a warming gateway is distinguishable from a stopped one
    void report_books(UnixNanos now) noexcept {
        if (now.ns < report_due_ns_ || state_->books.empty() || !has_logger()) {
            return;
        }
        report_due_ns_ = now.ns + k_book_report_interval_ns;
        // report only when seeds are queued or a book was also cold at the previous look
        const auto cold = cold_books();
        const bool behind = cold != 0 && (!state_->pending_seeds.empty() || cold_before_ != 0);
        cold_before_ = cold;
        if (!behind) {
            if (warned_) {
                warned_ = false;
                RLOG_INFO("aster: every one of {} depth books is seeded", state_->books.size());
            }
            return;
        }
        warned_ = true;
        RLOG_WARN("aster: {} of {} depth books have no seed yet, {} still queued — those "
                  "instruments publish no ladder until they do",
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
            RLOG_INFO("aster: {} depth books, all seeded — {} chain gap(s), {} reseed(s)",
                      state_->books.size(),
                      gaps,
                      reseeds);
            return;
        }
        RLOG_WARN("aster: {} of {} depth books NEVER SEEDED — {} chain gap(s), {} reseed(s)",
                  cold,
                  state_->books.size(),
                  gaps,
                  reseeds);
    }

    [[nodiscard]] std::size_t cold_books() const noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(state_->books, [](const auto& entry) { return !entry.second.live(); }));
    }

    // the process may not have installed a logger
    [[nodiscard]] static bool has_logger() noexcept { return ::raikiri::log::logger() != nullptr; }

    // one seed per tick; a refused seed keeps its queue place and next_seed_ns
    void drain_seeds(UnixNanos now) noexcept {
        if (state_->pending_seeds.empty() || now.ns < state_->next_seed_ns) {
            return;
        }
        const auto id = state_->pending_seeds.front();
        const auto entry = state_->books.find(id.packed);
        if (entry == state_->books.end()) {
            state_->pending_seeds.erase(state_->pending_seeds.begin());
            return;
        }
        const auto* listed = state_->instruments.find(id);
        if (listed == nullptr) {
            state_->pending_seeds.erase(state_->pending_seeds.begin());
            return;
        }
        if (!request_seed(*state_, entry->second, base_of(*listed))) {
            return; // left queued; the next tick asks again
        }
        state_->pending_seeds.erase(state_->pending_seeds.begin());
        state_->next_seed_ns = now.ns + k_seed_spacing_ns;
    }

    // one call covers every symbol
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns || in_flight_) {
            return;
        }
        in_flight_ = true;
        try {
            if (!state_->depth_rest.send(state_->builder.premium_index(),
                                         [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            in_flight_ = false;
            return;
        }
        // only a sent request advances the schedule
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
    }

    // uses the funding subscription list; aster has no separate turnover subscription
    void poll_ticker(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        try {
            if (!state_->depth_rest.send(state_->builder.ticker_24h(),
                                         [this](const net::RestResponse& response) { on_ticker(response); })) {
                ticker_in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        next_ticker_poll_ns_ = now.ns + k_ticker_poll_interval_ns;
    }

    void on_ticker(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            heed_rate_refusal(*state_, response);
            return;
        }
        static thread_local Ticker24hBuffer storage;
        const auto rows = parse_ticker_24h(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            const bool wanted = std::ranges::any_of(state_->funding_instruments,
                                                    [id](InstrumentId sub) { return sub == id; });
            if (!wanted) {
                continue;
            }
            if (const auto stats = map_ticker_stats(row, state_->instruments, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    void on_spot_ticker(const net::RestResponse& response) noexcept {
        state_->spot_tickers.answered();
        if (response.transport_failed() || response.status != 200) {
            heed_rate_refusal(*state_, response);
            return;
        }
        static thread_local Ticker24hBuffer storage;
        const auto rows = parse_ticker_24h(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto stats = map_ticker_stats(row, state_->instruments, received);
            if (stats.has_value() && state_->spot_tickers.wanted(stats->instrument)) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    void on_funding(const net::RestResponse& response) noexcept {
        in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            heed_rate_refusal(*state_, response);
            return;
        }
        static thread_local PremiumBuffer storage;
        const auto rows = parse_premium_index(response.body, storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto id = base_of(*instrument).id;
            const bool wanted = std::ranges::any_of(state_->funding_instruments,
                                                    [id](InstrumentId sub) { return sub == id; });
            if (!wanted) {
                continue;
            }
            if (const auto funding = map_funding(row, state_->instruments, received); funding.has_value()) {
                state_->sink->on_funding(*funding);
            }
        }
    }

    // called on every socket open; one frame carries at most the table's ws_frame
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> streams;
        streams.reserve(keys.size());
        for (const auto& key : keys) {
            streams.push_back(key);
        }
        // @bookTicker first; with paced sends the last streams wait longest
        std::ranges::stable_partition(streams, [](std::string_view stream) { return stream.ends_with("@bookTicker"); });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(std::span<const std::string_view>{streams}.subspan(
                built, std::min(net::frame_subscriptions(), streams.size() - built)));
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
    bool started_{false};
    bool in_flight_{false};
    bool ticker_in_flight_{false};
    bool warned_{false};
    std::uint64_t next_ticker_poll_ns_{0};
    std::uint64_t report_due_ns_{0};
    std::size_t cold_before_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<AsterData>(std::move(state));
}

} // namespace raikiri::venues::aster::internal
