#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
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
// second. Four a second leaves room for the pings sharing the same budget.
constexpr std::uint64_t k_subscribe_spacing_ns = 250ULL * 1'000'000ULL;

// A 24-hour rolling figure barely moves in a minute, and the whole table
// costs weight 40 against the funding read's 1. Slow on purpose.
constexpr std::uint64_t k_ticker_poll_interval_ns = 300ULL * 1'000'000'000ULL;

// Fires the REST read that seeds one book. The response lands on the gateway
// thread through IoContext::poll, so nothing here synchronises; a send that
// the pool refuses is left alone, because the next delta asks again.
void request_seed(VenueState& state, DepthBook& book, const InstrumentBase& base) {
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
        if (const auto book = target.snapshot(ts_event, response.received); book.has_value()) {
            owner->sink->on_depth(*book);
        }
    };
    if (state.depth_rest.send(
            state.builder.depth_snapshot(base.exchange_symbol.view(), k_seed_depth), handled)) {
        book.seed_requested();
    }
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
        request_seed(state, book, base);
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

    void on_open() override {
        std::vector<std::string_view> streams;
        streams.reserve(state_->desired_streams.size());
        for (const auto& stream : state_->desired_streams) {
            streams.push_back(stream);
        }
        // QUEUED, not sent. The pacing lives in poll(); see state.hpp for what
        // sending them all at once cost.
        state_->pending_subscribes.clear();
        state_->next_subscribe_ns = 0;
        std::size_t built = 0;
        while (built < streams.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{streams}.subspan(built));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            state_->pending_subscribes.emplace_back(frame->text);
            built += frame->consumed;
        }
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

    void attach(net::WsClient& client) noexcept { client_ = &client; }

private:
    VenueState* state_;
    net::WsClient* client_{nullptr};
};

class BinanceData final : public exec::DataClient {
public:
    explicit BinanceData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          client_{state_->io,
                  net::WsConfig{.host = std::string{stream_host(state_->testnet)},
                                .port = "443",
                                .target = std::string{stream_path()}},
                  handler_} {
        handler_.attach(client_);
    }

    std::expected<void, exec::VenueError> start() override {
        state_->depth_rest.start();
        client_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        client_.stop();
        state_->depth_rest.stop();
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
        if (state_->desired_streams.size() >= k_max_streams_per_connection) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "stream limit reached")};
        }
        state_->add_stream(stream);
        if (client_.connected()) {
            const std::array<std::string_view, 1> one{stream};
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
        drain_subscribes(now);
        poll_funding(now);
        poll_ticker(now);
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
    }

private:
    // One SUBSCRIBE per tick, well under the venue's allowance. A catalogue of
    // 1024 streams therefore takes about eight seconds to ask for, which is
    // the price of the venue answering at all.
    void drain_subscribes(UnixNanos now) noexcept {
        if (state_->pending_subscribes.empty() || now.ns < state_->next_subscribe_ns) {
            return;
        }
        if (!client_.connected()) {
            return; // the reconnect refills the queue
        }
        if (!client_.send(state_->pending_subscribes.front())) {
            return; // wedged socket; the reconnect retries the whole set
        }
        state_->pending_subscribes.erase(state_->pending_subscribes.begin());
        state_->next_subscribe_ns = now.ns + k_subscribe_spacing_ns;
    }

    // One call covers every symbol, so there is no cursor and no per-symbol
    // fan-out: the whole table refreshes on each poll and only the instruments
    // somebody subscribed reach the sink.
    void poll_funding(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < state_->next_funding_poll_ns ||
            in_flight_) {
            return;
        }
        state_->next_funding_poll_ns = now.ns + k_funding_poll_interval_ns;
        in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.premium_index(),
                    [this](const net::RestResponse& response) { on_funding(response); })) {
                in_flight_ = false;
            }
        } catch (...) {
            in_flight_ = false;
        }
    }

    // Rides the funding subscription's instrument list: binance has no
    // separate way to say "I want turnover for this symbol", and a gateway
    // that subscribed to neither should not be paying weight for either.
    void poll_ticker(UnixNanos now) noexcept {
        if (state_->funding_instruments.empty() || now.ns < next_ticker_poll_ns_ ||
            ticker_in_flight_) {
            return;
        }
        next_ticker_poll_ns_ = now.ns + k_ticker_poll_interval_ns;
        ticker_in_flight_ = true;
        try {
            if (!state_->depth_rest.send(
                    state_->builder.ticker_24h(),
                    [this](const net::RestResponse& response) { on_ticker(response); })) {
                ticker_in_flight_ = false;
            }
        } catch (...) {
            ticker_in_flight_ = false;
        }
    }

    void on_ticker(const net::RestResponse& response) noexcept {
        ticker_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
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

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsClient client_;
    bool started_{false};
    bool in_flight_{false};
    bool ticker_in_flight_{false};
    std::uint64_t next_ticker_poll_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BinanceData>(std::move(state));
}

} // namespace raikiri::venues::binance::detail
