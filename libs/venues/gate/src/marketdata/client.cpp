#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::gate::internal {

namespace {

// gate closes a connection after 60 s without application traffic
constexpr std::uint64_t k_app_ping_interval_ns = 20'000'000'000ULL;
constexpr std::uint64_t k_rest_seed_interval_ns = 1'000'000'000ULL;

// fires the rest read that seeds one book; a refused send is left for the next delta
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
    const auto id = base.id;
    auto* owner = &state;
    const auto handled = [owner, id](net::RestResponse&& response) {
        const auto entry = owner->books.find(id.packed);
        if (entry == owner->books.end()) {
            return;
        }
        auto& target = entry->second;
        if (response.transport_failed() || response.status < 200 || response.status >= 300) {
            target.seed_failed();
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_order_book(response.body, storage);
        if (!wire.has_value()) {
            target.seed_failed();
            return;
        }
        if (target.seed(*wire) != BookResult::Applied) {
            return;
        }
        if (const auto seeded = target.snapshot(s_to_unix(wire->update_s), response.received); seeded.has_value()) {
            owner->sink->on_depth(*seeded);
        }
    };
    if (state.depth_rest.send(state.builder.order_book(k_settle_usdt, base.exchange_symbol.view(), k_depth_limit),
                              handled)) {
        book.seed_requested();
    }
}

void handle_book_update(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_update(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    const auto* instrument = state.instruments.find(wire->contract);
    if (instrument == nullptr) {
        return;
    }
    const auto& base = base_of(*instrument);
    const auto entry = state.books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first;
    auto& book = entry->second;
    // asked before the delta is offered so the triggering frame buffers behind it
    if (book.wants_seed()) {
        request_seed(state, book, base);
    }
    if (book.apply(*wire) != BookResult::Applied) {
        return; // unseeded, stale or resynchronising: publish nothing
    }
    // checked before building the snapshot, which is the expensive part
    if (!state.sink->wants_depth(base.id, received)) {
        return;
    }
    if (const auto snapshot = book.snapshot(ms_to_unix(wire->ts_ms, UnixNanos{}), received); snapshot.has_value()) {
        state.sink->on_depth(*snapshot);
    }
}

// spot.order_book republishes the whole depth, so no seed or chain is needed
void handle_book_snapshot(VenueState& state, std::string_view frame, UnixNanos received) {
    static thread_local LevelBuffer storage;
    const auto wire = parse_book_snapshot(frame, storage);
    if (!wire.has_value()) {
        return;
    }
    if (const auto book = map_book(*wire, state.instruments, received); book.has_value()) {
        state.sink->on_depth(*book);
    }
}

// the held name of an option the wire spells in full; nullopt off the option market
[[nodiscard]] std::optional<SymbolText> held_name(const VenueState& state, std::string_view contract) {
    return state.market == MarketType::Option ? option_symbol(contract) : std::nullopt;
}

void handle_option_ticker(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto wire = parse_option_ticker(frame);
    if (!wire.has_value()) {
        return;
    }
    const auto greeks = map_option_greeks(*wire, state.instruments, received);
    if (!greeks.has_value()) {
        return;
    }
    state.sink->on_option_greeks(*greeks);
    state.cover(greeks->instrument, 2);
    if (!state.option_quote_due.contains(greeks->instrument.packed)) {
        return;
    }
    if (const auto bbo = map_option_quote(*wire, state.instruments, received); bbo.has_value()) {
        state.sink->on_bbo(*bbo);
        if (!bbo->bid.is_undef() && !bbo->ask.is_undef()) {
            state.cover(bbo->instrument, 1);
        }
    }
}

void handle_frame(VenueState& state, std::string_view frame, UnixNanos received) {
    const auto event = parse_event(frame);
    if (!event.has_value()) {
        // unknown shapes are ignored
        return;
    }
    switch (event->kind) {
    case EventKind::BookTicker: {
        auto wire = parse_book_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto held = held_name(state, wire->contract);
        if (held.has_value()) {
            wire->contract = held->view();
        }
        auto bbo = map_bbo(*wire, state.instruments, received);
        if (!bbo.has_value()) {
            return;
        }
        state.quote_seed_due.erase(bbo->instrument.packed);
        if (held.has_value()) {
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
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(frame, trades, state.market);
        if (!wire.has_value()) {
            return;
        }
        for (auto entry : *wire) {
            const auto held = held_name(state, entry.contract);
            if (held.has_value()) {
                entry.contract = held->view();
            }
            if (const auto trade = map_trade(entry, state.instruments, received); trade.has_value()) {
                state.sink->on_trade(*trade);
                state.cover(trade->instrument, 4);
            }
        }
        break;
    }
    case EventKind::Ticker: {
        if (state.market == MarketType::Option) {
            handle_option_ticker(state, frame, received);
            break;
        }
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, state.instruments, received); funding.has_value()) {
            state.funding_seed_due.erase(funding->instrument.packed);
            state.sink->on_funding(*funding);
        }
        if (const auto stats = map_ticker_stats(*wire, state.instruments, received); stats.has_value()) {
            state.sink->on_ticker_stats(*stats);
        }
        break;
    }
    case EventKind::BookUpdate:
        handle_book_update(state, frame, received);
        break;
    case EventKind::BookSnapshot:
        handle_book_snapshot(state, frame, received);
        break;
    case EventKind::Error: {
        if (const auto err = parse_stream_error(frame); err.has_value()) {
            state.last_error_code = err->code;
            ++state.subscribe_refusals;
        }
        break;
    }
    case EventKind::OrderUpdate:
    case EventKind::UserTrade:
    case EventKind::Pong:
    case EventKind::SubscribeAck:
    case EventKind::Unknown:
        break; // private kinds and control frames are not this socket's
    }
}

// a pool key is channel and contract joined by a byte neither contains
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view contract) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(contract);
    return key;
}

// the only decode; views into the pool-owned key
[[nodiscard]] std::optional<Subscription> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Subscription{.channel = key.substr(0, cut), .contract = key.substr(cut + 1)};
}

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(VenueState& state) : state_{&state} {}

    // each socket re-asks for its own keys on open; its books are invalidated by the pool's open hook
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

class GateData final : public exec::DataClient {
public:
    explicit GateData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{public_host(state_->testnet, state_->market)},
                              .port = std::string{stream_port()},
                              .target = std::string{public_path(state_->testnet, state_->market)}},
                handler_,
                // gate caps a connection at 1024 channels; it drops a socket carrying every option
                // underlying, so options take three channels a socket
                net::WsPoolConfig{.key_cap = state_->market == MarketType::Option ? 3U : 0U},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) {
                    invalidate_books(keys);
                    due_rest_seeds(keys);
                }} {
        meter_market_data(*state_, state_->depth_rest);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        state_->depth_rest.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        if (instrument.market() != state_->market) {
            // gate spells a spot pair and its perp identically, so one socket serves one market
            return std::unexpected{exec::not_implemented("gate: this socket serves one market")};
        }
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind, state_->market);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const bool option = state_->market == MarketType::Option;
        if (option && kind == exec::SubKind::Bbo) {
            state_->option_quote_due.insert(instrument.packed);
        }
        const auto target = option ? option_underlying(symbol) : std::string{symbol};
        const auto before = state_->desired.size();
        state_->add_subscription(channel, target);
        if (state_->desired.size() == before) {
            return {}; // refcounting lives in the gateway core; this is a set
        }
        // the pool places it, opening another socket when needed
        const std::array<std::string, 1> one{key_of(channel, target)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "gate: connection pool is full")};
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
            pool_.broadcast(state_->protocol.ping_frame(now.ns / 1'000'000'000ULL, state_->market));
            state_->next_app_ping_ns = now.ns + k_app_ping_interval_ns;
        }
        seed_from_rest(now);
        report_coverage(now);
    }

private:
    void report_coverage(UnixNanos now) {
        if (state_->market != MarketType::Option || now.ns < state_->next_coverage_ns) {
            return;
        }
        const bool first = state_->next_coverage_ns == 0;
        state_->next_coverage_ns = now.ns + 60'000'000'000ULL;
        if (first || ::raikiri::log::logger() == nullptr) {
            return;
        }
        std::array<std::size_t, 3> counts{};
        for (const auto bits : state_->option_coverage) {
            for (std::size_t bit = 0; bit < counts.size(); ++bit) {
                counts[bit] += (bits >> bit) & 1U;
            }
        }
        RLOG_INFO("gate option: {} of {} quoted two-sided, {} with greeks, {} traded; {} sockets, {} keys",
                  counts[0],
                  state_->instruments.size(),
                  counts[1],
                  counts[2],
                  pool_.connection_count(),
                  pool_.key_count());
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;

    // only this socket's depth books: invalidating all of them sends the whole catalogue to rest at once
    void invalidate_books(std::span<const std::string> keys) {
        const auto depth = channel_for(exec::SubKind::Depth, state_->market);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || one->channel != depth) {
                continue;
            }
            const auto* instrument = state_->instruments.find(one->contract);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*instrument).id.packed); entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    void due_rest_seeds(std::span<const std::string> keys) {
        const auto quote = channel_for(exec::SubKind::Bbo, state_->market);
        const auto funding = channel_for(exec::SubKind::Funding, state_->market);
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || (one->channel != quote && one->channel != funding)) {
                continue;
            }
            if (const auto* instrument = state_->instruments.find(one->contract); instrument != nullptr) {
                const auto packed = base_of(*instrument).id.packed;
                (one->channel == quote ? state_->quote_seed_due : state_->funding_seed_due).insert(packed);
            }
        }
    }

    // one bulk call per kind for contracts still due, at most once a second
    void seed_from_rest(UnixNanos now) noexcept {
        if (state_->market != MarketType::Perp || now.ns < next_seed_ns_) {
            return;
        }
        next_seed_ns_ = now.ns + k_rest_seed_interval_ns;
        try {
            if (!state_->quote_seed_due.empty() && !quote_seed_in_flight_) {
                quote_seed_in_flight_ = state_->depth_rest.send(
                    state_->builder.tickers(k_settle_usdt),
                    [this](net::RestResponse&& response) { on_quote_seed(response); });
            }
            if (!state_->funding_seed_due.empty() && !funding_seed_in_flight_) {
                funding_seed_in_flight_ = state_->depth_rest.send(
                    state_->builder.contracts(k_settle_usdt),
                    [this](net::RestResponse&& response) { on_funding_seed(response); });
            }
        } catch (...) {
            return;
        }
    }

    void on_quote_seed(const net::RestResponse& response) {
        quote_seed_in_flight_ = false;
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerQuoteBuffer>();
        const auto rows = parse_futures_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto bbo = map_bbo(row, state_->instruments, response.received);
            if (bbo.has_value() && state_->quote_seed_due.erase(bbo->instrument.packed) > 0) {
                state_->sink->on_bbo(*bbo);
            }
        }
        state_->quote_seed_due.clear();
    }

    void on_funding_seed(const net::RestResponse& response) {
        funding_seed_in_flight_ = false;
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<ContractBuffer>();
        const auto rows = parse_contracts(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto funding = map_funding(row, state_->instruments, response.received);
            if (funding.has_value() && state_->funding_seed_due.erase(funding->instrument.packed) > 0) {
                state_->sink->on_funding(*funding);
            }
        }
        state_->funding_seed_due.clear();
    }

    // called on every socket open, so the quote channel goes first here
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<Subscription> views;
        views.reserve(keys.size());
        for (const auto& key : keys) {
            if (const auto one = split_key(key); one.has_value()) {
                views.push_back(*one);
            }
        }
        std::ranges::stable_partition(views,
                                      [](const Subscription& one) { return one.channel.ends_with("book_ticker"); });

        std::vector<std::string> frames;
        const auto now_s = state_->now_s();
        std::size_t built = 0;
        while (built < views.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const Subscription>{views}.subspan(
                    built, std::min(net::frame_subscriptions(), views.size() - built)),
                now_s);
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    net::WsPool pool_;
    std::uint64_t next_seed_ns_{0};
    bool quote_seed_in_flight_{false};
    bool funding_seed_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<GateData>(std::move(state));
}

} // namespace raikiri::venues::gate::internal
