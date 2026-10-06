#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::binance::internal {

namespace {

// the running rate is republished continuously; a minute is fine enough
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

// spacing only smooths the burst; the admit gate bounds the rate
constexpr std::uint64_t k_seed_spacing_ns = 25ULL * 1'000'000ULL;

// how often still-cold books are reported
constexpr std::uint64_t k_book_report_interval_ns = 15ULL * 1'000'000'000ULL;

// the 24h table costs weight 40, 80 on spot; once a minute stays well inside either budget
constexpr std::uint64_t k_ticker_poll_interval_ns = 60ULL * 1'000'000'000ULL;

void add_once(std::vector<InstrumentId>& ids, InstrumentId id) {
    if (std::ranges::find(ids, id) == ids.end()) {
        ids.push_back(id);
    }
}

// applies a 429 or 418 to the shared market-data pool; a ban arrives as 418 with a deadline body
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

// fires the rest read that seeds one book; a refused send is left for the next delta
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
        switch (classify_seed_answer(response.status, response.body)) {
        case SeedAnswer::Book:
            break;
        case SeedAnswer::RateLimited:
            heed_rate_refusal(*owner, response);
            target.seed_failed();
            return;
        case SeedAnswer::Retry:
            target.seed_failed();
            return;
        case SeedAnswer::NoBook: {
            target.seed_refused();
            if (::raikiri::log::logger() != nullptr) {
                const auto* listed = owner->instruments.find(id);
                RLOG_WARN("binance: {} depth snapshot refused with {} ({}) — no book for it "
                          "until the catalogue is reloaded",
                          listed != nullptr ? base_of(*listed).exchange_symbol.view() : std::string_view{"?"},
                          response.status,
                          response.body.substr(0, 120));
            }
            return;
        }
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
        // refused: undo the in-flight mark or the book waits forever
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
    // asked before the delta is offered so the triggering frame buffers behind it
    if (book.wants_seed()) {
        // queued; poll() paces the sends
        if (std::ranges::find(state.pending_seeds, base.id) == state.pending_seeds.end()) {
            state.pending_seeds.push_back(base.id);
        }
        book.seed_requested();
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: publish nothing
    }
    // checked before building the snapshot, which is the expensive part
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->event_ms, UnixNanos{}), received); snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

// one frame carries every option of an underlying; a book ticker not heard from yet is quoted from it
void handle_option_marks(VenueState& state, std::string_view data, UnixNanos received) {
    static thread_local const auto storage = std::make_unique<OptionMarkBuffer>();
    const auto rows = parse_option_marks(data, *storage);
    if (!rows.has_value()) {
        return;
    }
    for (const auto& row : *rows) {
        if (const auto greeks = map_option_greeks(row, state.instruments, received); greeks.has_value()) {
            state.sink->on_option_greeks(*greeks);
            state.cover(greeks->instrument, 2);
            if (!state.option_quote_due.contains(greeks->instrument.packed)) {
                continue;
            }
        }
        if (const auto bbo = map_option_quote(row, state.instruments, received); bbo.has_value()) {
            state.sink->on_bbo(*bbo);
            if (!bbo->bid.is_undef() && !bbo->ask.is_undef()) {
                state.cover(bbo->instrument, 1);
            }
        }
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    // unwrap once and read the event from the payload
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
    }
    if (data->starts_with('[')) {
        handle_option_marks(state, *data, received);
        return;
    }
    const auto event = parse_event_payload(*data);
    if (!event.has_value()) {
        return; // unknown shapes are ignored: venues add frames
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        const auto wire = parse_book_ticker(*data);
        if (!wire.has_value()) {
            return;
        }
        auto bbo = map_bbo(*wire, state.instruments, received);
        if (!bbo.has_value()) {
            return;
        }
        if (bbo->instrument.market() == MarketType::Option) {
            blank_empty_sides(*bbo);
            state.option_quote_due.erase(bbo->instrument.packed);
            if (!bbo->bid.is_undef() && !bbo->ask.is_undef()) {
                state.cover(bbo->instrument, 1);
            }
        }
        state.sink->on_bbo(*bbo);
        break;
    }
    case EventKind::Trade: {
        const auto wire = parse_trade(*data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, state.instruments, received); trade.has_value()) {
            state.sink->on_trade(*trade);
            state.cover(trade->instrument, 4);
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

    // each socket in the pool re-asks for its own streams when it opens
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

class BinanceData final : public exec::DataClient {
public:
    explicit BinanceData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{make_pool(stream_path(exec::SubKind::Bbo, state_->market), 0)},
          // binance drops a socket carrying several underlyings' marks, so each gets its own
          market_pool_{make_pool(stream_path(exec::SubKind::MarkPrice, state_->market),
                                 state_->market == MarketType::Option ? 1 : 0)} {
        // seeds, funding and the 24h table share this pool and the order path's weight
        meter_market_data(*state_, state_->depth_rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        pool_.start();
        market_pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        market_pool_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected() || market_pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding && state_->market == MarketType::Perp) {
            add_once(state_->funding_instruments, instrument);
            add_once(state_->ticker_instruments, instrument);
            return {};
        }
        std::array<char, 64> buf{};
        const auto stream = stream_name(kind, symbol, buf, state_->market);
        if (stream.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->spot()) {
            add_once(state_->ticker_instruments, instrument);
        }
        if (state_->market == MarketType::Option && kind == exec::SubKind::Bbo) {
            state_->option_quote_due.insert(instrument.packed);
        }
        const auto before = state_->desired_streams.size();
        state_->add_stream(stream);
        if (state_->desired_streams.size() == before) {
            return {}; // already placed on a socket; a stream is asked for once
        }
        // the pool places it, opening another socket when needed
        const std::array<std::string, 1> one{std::string{stream}};
        auto& pool = stream_path(kind, state_->market) == stream_path(exec::SubKind::Bbo, state_->market)
                         ? pool_
                         : market_pool_;
        if (!pool.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "binance: connection pool is full")};
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
        market_pool_.poll(now);
        drain_seeds(now);
        report_books(now);
        poll_funding(now);
        poll_ticker(now);
        report_coverage(now);
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    void report_coverage(UnixNanos now) {
        if (state_->market != MarketType::Option || now.ns < state_->next_coverage_ns) {
            return;
        }
        const bool first = state_->next_coverage_ns == 0;
        state_->next_coverage_ns = now.ns + 60'000'000'000ULL;
        if (first || !has_logger()) {
            return;
        }
        std::array<std::size_t, 3> counts{};
        for (const auto bits : state_->option_coverage) {
            for (std::size_t bit = 0; bit < counts.size(); ++bit) {
                counts[bit] += (bits >> bit) & 1U;
            }
        }
        RLOG_INFO("binance option: {} of {} quoted two-sided, {} with greeks, {} traded; {}+{} sockets",
                  counts[0],
                  state_->instruments.size(),
                  counts[1],
                  counts[2],
                  pool_.connection_count(),
                  market_pool_.connection_count());
    }

    // a full-catalogue start cannot seed every book within a minute's allowance; this reports progress
    void report_books(UnixNanos now) noexcept {
        if (now.ns < report_due_ns_ || state_->books.empty() || !has_logger()) {
            return;
        }
        report_due_ns_ = now.ns + k_book_report_interval_ns;
        // behind when seeds are still queued or something was cold at the previous look too
        const auto cold = cold_books();
        const auto refused = refused_books();
        const bool behind = cold != 0 && (!state_->pending_seeds.empty() || cold_before_ != 0);
        cold_before_ = cold;
        if (!behind) {
            if (warned_) {
                warned_ = false;
                RLOG_INFO("binance: every one of {} depth books is seeded, {} refused by the venue",
                          state_->books.size() - refused,
                          refused);
            }
            return;
        }
        warned_ = true;
        RLOG_WARN("binance: {} of {} depth books have no seed yet, {} still queued, {} refused "
                  "by the venue — those instruments publish no ladder until they do",
                  cold,
                  state_->books.size() - refused,
                  state_->pending_seeds.size(),
                  refused);
    }

    // the only reader of gaps() and reseeds()
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
        const auto refused = refused_books();
        if (cold == 0) {
            RLOG_INFO("binance: {} depth books, all seeded, {} refused by the venue — {} chain "
                      "gap(s), {} reseed(s)",
                      state_->books.size() - refused,
                      refused,
                      gaps,
                      reseeds);
            return;
        }
        RLOG_WARN("binance: {} of {} depth books NEVER SEEDED, {} refused by the venue — {} chain "
                  "gap(s), {} reseed(s)",
                  cold,
                  state_->books.size() - refused,
                  refused,
                  gaps,
                  reseeds);
    }

    [[nodiscard]] std::size_t cold_books() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            state_->books, [](const auto& entry) { return !entry.second.live() && !entry.second.refused(); }));
    }

    [[nodiscard]] std::size_t refused_books() const noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(state_->books, [](const auto& entry) { return entry.second.refused(); }));
    }

    // a library may run without a logger installed
    [[nodiscard]] static bool has_logger() noexcept { return ::raikiri::log::logger() != nullptr; }

    // a refused seed keeps its queue place and next_seed_ns, so the next tick asks again
    void drain_seeds(UnixNanos now) noexcept {
        if (state_->pending_seeds.empty() || now.ns < state_->next_seed_ns) {
            return;
        }
        const auto id = state_->pending_seeds.front();
        const auto entry = state_->books.find(id.packed);
        if (entry == state_->books.end() || entry->second.refused()) {
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

    // one call covers every symbol; only subscribed instruments reach the sink
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
        // only a call actually sent advances the schedule
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
    }

    void poll_ticker(UnixNanos now) noexcept {
        if (state_->ticker_instruments.empty() || now.ns < next_ticker_poll_ns_ || ticker_in_flight_) {
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
            if (std::ranges::find(state_->ticker_instruments, id) == state_->ticker_instruments.end()) {
                continue;
            }
            if (const auto stats = map_ticker_stats(row, state_->instruments, received); stats.has_value()) {
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

    // binance caps one connection at 1024 streams
    net::WsPool make_pool(std::string_view target, std::size_t key_cap) {
        return net::WsPool{state_->io,
                           net::WsConfig{.host = std::string{stream_host(state_->testnet, state_->market)},
                                         .port = "443",
                                         .target = std::string{target}},
                           handler_,
                           net::WsPoolConfig{.key_cap = key_cap},
                           [this](std::span<const std::string> keys) { return build_frames(keys); }};
    }

    // called on every socket open, so the order here is the order the venue is asked in
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> streams;
        streams.reserve(keys.size());
        for (const auto& key : keys) {
            streams.push_back(key);
        }
        // bookTicker first: with paced sends the last streams go out late
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
    net::WsPool market_pool_;
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
    return std::make_unique<BinanceData>(std::move(state));
}

} // namespace raikiri::venues::binance::internal
