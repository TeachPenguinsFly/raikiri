#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
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

namespace raikiri::venues::binance::detail {

namespace {

// State every binance connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
// Binance settles funding every eight hours and republishes the running rate
// continuously; a poll a minute is far finer than the fact changes.
constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;

// Binance drops a connection that sends more than about five messages a
// second, counted per socket. Four a second leaves room for the pings sharing
// the same budget.
constexpr std::uint64_t k_subscribe_spacing_ns = 250ULL * 1'000'000ULL;

// Enough sockets for binance's whole catalogue AT THE PACKING ws.hpp CHOOSES,
// and the two numbers have to be read together. 571 USD-M listings times the
// three streams the gateway asks for — @bookTicker, @trade and @depth@<cadence>
// — is 1713. At k_max_streams_per_connection that is six sockets, and eight
// leaves room for the listings binance adds.
//
// LOWERING THE PACKING WITHOUT RAISING THIS IS A SILENT COVERAGE LOSS: four
// sockets at 300 keys is a ceiling of 1200 against 1713 asked for, and the
// pool refuses the rest. Measured that way: 400 depth books of 571, and 377 of
// 538 instruments quoting, with no error anywhere but the refusal counter.
//
// Funding is not a fourth stream: @markPrice is acknowledged here and then
// never sends, so the rate is pulled over REST instead.
constexpr std::size_t k_max_connections = 8;

// SPACING IS NOT THE CEILING — the rate budget is. Forty seeds a second at
// weight 5 is 12000 a minute against an allowance of 2400, so this only keeps
// the queue from arriving as one burst; what actually bounds it is the admit
// gate installed in the constructor.
//
// That was a claim and not a fact until the seed pool was widened: two warm
// connections bounded it at seven a second instead, and the gate never refused
// anything. See VenueState::depth_rest for the measurement.
constexpr std::uint64_t k_seed_spacing_ns = 25ULL * 1'000'000ULL;

// How often a gateway whose books are still cold says so. Long enough to be
// silence on a healthy start, short enough to answer someone who is looking at
// an empty depth column and about to go hunting for a bug.
constexpr std::uint64_t k_book_report_interval_ns = 15ULL * 1'000'000'000ULL;

// A 24-hour rolling figure barely moves in a minute, and the whole table
// costs weight 40 against the funding read's 1. Slow on purpose.
constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

// THE VENUE'S OWN REFUSAL, APPLIED TO THE POOL EVERY MARKET-DATA CALL DRAWS
// FROM.
//
// The private half of this adapter has always done this — see rest/api.hpp —
// and the market-data half never did, so a 429 on a book seed was read as one
// failed snapshot and the budget went on believing it had weight to sell. That
// is the path that earned this IP a ban twice on 2026-09-06: binance sends 429
// before it sends 418, and nothing here was listening to the first.
//
// The STATUS is read as well as the body, because a ban arrives as 418 with a
// body carrying a deadline rather than a code this parser knows, and an unread
// ban is the whole failure.
//
// One second, matching what the private half holds for. It drains the bucket,
// so what resumes afterwards is the refill rate — forty weight a second, or
// eight seeds — which is the pace binance will actually sell.
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
    state.budget->penalize(exec::RateBucket::Shared, state.clock->mono(), 1'000'000'000ULL);
}

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send that the
// pool or the rate budget refuses is left alone, because the next delta asks
// again.
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
        if (const auto err = parse_rest_error(response.body);
            err.has_value() && err->code != k_ok) {
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
        const auto ts_event = ms_to_unix(wire->transaction_ms, response.received);
        // NOT `book`: the enclosing function takes a DepthBook& by that name,
        // and shadowing it here reads as the ladder rather than the snapshot
        // taken from it.
        if (const auto seeded = target.snapshot(ts_event, response.received); seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    if (!state.depth_rest.send(
            state.builder.depth_snapshot(base.exchange_symbol.view(), k_seed_depth), handled)) {
        // Refused — a full pool, or a budget with no weight left to sell. The
        // caller marked the book in flight when it queued the seed, so that has
        // to be undone or the book waits for a request nobody made.
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
    const auto entry =
        state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // Asked before the delta is offered, so the frame that triggered the seed
    // is itself buffered behind it.
    if (book.wants_seed()) {
        // QUEUED, not fired. The pacing lives in poll(); see state.hpp for what
        // firing them all at once cost.
        if (std::ranges::find(state.pending_seeds, base.id) == state.pending_seeds.end()) {
            state.pending_seeds.push_back(base.id);
        }
        book.seed_requested();
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: silence, never a guess
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->event_ms, received), received);
        snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        return; // unknown shapes are ignored, never fatal: venues add frames
    }
    const auto data = unwrap(frame);
    if (!data.has_value()) {
        return;
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
        if (const auto funding = map_funding(*wire, state.instruments, received);
            funding.has_value()) {
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

    // NOTHING IS RESUBSCRIBED HERE. Each socket in the pool re-asks for its
    // own streams when it opens; replaying the whole desired set on every one
    // of them is the flood the pool exists to avoid.
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
          pool_{state_->io,
                net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{stream_path()}},
                handler_,
                // Binance caps one CONNECTION at 1024 streams; the pool turns
                // that into a per-socket number instead of the process's,
                // which is what left 342 of 571 listings subscribed and 229
                // refused outright.
                net::WsPoolConfig{.max_keys_per_connection = k_max_streams_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {
        // The seeds, the funding pull and the 24-hour table all ride this one
        // pool, and binance counts every one of them against the same weight
        // the order path spends.
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

    // What this venue's sockets were told no about. Counted by the
    // adapter since it was written; this is what finally reads it.
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
        // THE POOL PLACES IT. A stream that does not fit on any existing socket
        // opens another, and only the pool's own ceiling refuses — which is
        // said out loud rather than leaving the instrument silently unwatched.
        const std::array<std::string, 1> one{std::string{stream}};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "binance: connection pool is full")};
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
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    // A BOOK THAT NEVER SEEDS IS SILENT, AND SILENCE READS AS A DEAD VENUE.
    //
    // The whole catalogue is 571 seeds at weight 5 — 2855 against an allowance
    // of 2400 a minute — so a full-catalogue start CANNOT have its books up
    // inside one minute's allowance whatever it does, and spends that minute
    // with most instruments publishing no ladder. That is what the venue
    // sells and not a fault. What WAS a fault is that nothing said so: every
    // depth column on the board was blank, `depth=` in the shutdown line was
    // already in the hundreds of thousands from the handful of books that had
    // come up, and gaps() and reseeds() were counted here and read by nobody.
    // A warming gateway and a stopped one looked identical.
    void report_books(UnixNanos now) noexcept {
        if (now.ns < report_due_ns_ || state_->books.empty() || !has_logger()) {
            return;
        }
        report_due_ns_ = now.ns + k_book_report_interval_ns;
        // BEHIND, NOT MERELY RESYNCHRONISING. A book that lost its chain is
        // cold for one round trip, and saying so every fifteen seconds is the
        // check that cries wolf — and an ignored check is worse than none. The
        // venue is BEHIND when seeds are still queued, or when something was
        // cold at the previous look too and has therefore been cold for a
        // quarter of a minute.
        const auto cold = cold_books();
        const bool behind = cold != 0 && (!state_->pending_seeds.empty() || cold_before_ != 0);
        cold_before_ = cold;
        if (!behind) {
            if (warned_) {
                warned_ = false;
                RLOG_INFO("binance: every one of {} depth books is seeded", state_->books.size());
            }
            return;
        }
        warned_ = true;
        RLOG_WARN("binance: {} of {} depth books have no seed yet, {} still queued — those "
                  "instruments publish no ladder until they do",
                  cold,
                  state_->books.size(),
                  state_->pending_seeds.size());
    }

    // The last word on the books, and the only reader gaps() and reseeds() have.
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
            RLOG_INFO("binance: {} depth books, all seeded — {} chain gap(s), {} reseed(s)",
                      state_->books.size(),
                      gaps,
                      reseeds);
            return;
        }
        RLOG_WARN("binance: {} of {} depth books NEVER SEEDED — {} chain gap(s), {} reseed(s)",
                  cold,
                  state_->books.size(),
                  gaps,
                  reseeds);
    }

    [[nodiscard]] std::size_t cold_books() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            state_->books, [](const auto& entry) { return !entry.second.live(); }));
    }

    // A LIBRARY MAY NOT ASSUME THE PROCESS INSTALLED A LOGGER, and RLOG_*
    // dereferences it. Every other library here reports through a callback the
    // app hands it, and there is none for this: the gateway asks an adapter to
    // subscribe, hears the refusal, and then never asks it anything again.
    [[nodiscard]] static bool has_logger() noexcept { return ::raikiri::log::logger() != nullptr; }

    // One book seed per tick, and only while the budget will still sell one.
    // A refused seed keeps its place in the queue AND leaves `next_seed_ns`
    // where it was, so the next tick asks again rather than the book waiting
    // for a request nobody made.
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

    // One call covers every symbol, so there is no cursor and no per-symbol
    // fan-out: the whole table refreshes on each poll and only the instruments
    // somebody subscribed reach the sink.
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns ||
            in_flight_) {
            return;
        }
        in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.premium_index(),
                    [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            in_flight_ = false;
            return;
        }
        // Only a call the venue was actually asked buys the next minute.
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
    }

    // Rides the funding subscription's instrument list: binance has no
    // separate way to say "I want turnover for this symbol", and a gateway
    // that subscribed to neither should not be paying weight for either.
    void poll_ticker(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < next_ticker_poll_ns_ ||
            ticker_in_flight_) {
            return;
        }
        ticker_in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.ticker_24h(),
                    [this](const net::RestResponse& response) { on_ticker(response); })) {
                ticker_in_flight_ = false;
                return; // refused: still due, so the next tick asks again
            }
        } catch (...) {
            ticker_in_flight_ = false;
            return;
        }
        // Only a call the venue was actually asked buys the next five minutes.
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
            if (const auto stats = map_ticker_stats(row, state_->instruments, received);
                stats.has_value()) {
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
            if (const auto funding = map_funding(row, state_->instruments, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
            }
        }
    }

    // A pool key is one stream string. Binance's desired set is already a flat
    // list of stream names, so nothing is encoded into the key and taken apart
    // here.
    //
    // Called on every open of ONE socket, which is why the ordering lives here
    // rather than at subscribe time: on a reconnect the order that matters is
    // the order the venue is asked in. One frame carries at most
    // k_max_streams_per_frame, so a full socket is 32 frames and eight seconds.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> streams;
        streams.reserve(keys.size());
        for (const auto& key : keys) {
            streams.push_back(key);
        }
        // THE QUOTE FIRST, then the rest. Once the sends are paced, whatever
        // goes last effectively does not go at all — and `@bookTicker` is this
        // venue's top of book: it is the suffix stream_name() builds for
        // SubKind::Bbo, and it feeds the Bid/Ask columns every screen shows.
        std::ranges::stable_partition(
            streams, [](std::string_view stream) { return stream.ends_with("@bookTicker"); });

        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{streams}.subspan(built));
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
    return std::make_unique<BinanceData>(std::move(state));
}

} // namespace raikiri::venues::binance::detail
