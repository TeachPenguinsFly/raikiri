#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/cryptocom/book.hpp>
#include <raikiri/venues/cryptocom/endpoints.hpp>
#include <raikiri/venues/cryptocom/envelope.hpp>
#include <raikiri/venues/cryptocom/map.hpp>
#include <raikiri/venues/cryptocom/market_data.hpp>
#include <raikiri/venues/cryptocom/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::cryptocom::detail {

namespace {

constexpr std::uint64_t k_subscribe_spacing_ns = 50ULL * 1'000'000ULL;

// A perpetual costs six channels and a pair three: the full perp catalogue
// needs six sockets at 400. Lowering this refuses instruments.
constexpr std::size_t k_max_connections = 8;

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

class CryptoComData;

class PublicHandler final : public net::WsHandler {
public:
    explicit PublicHandler(CryptoComData& owner) : owner_{&owner} {}

    void on_open() override;
    void on_message(std::string_view payload, UnixNanos received) noexcept override;
    void on_close(net::WsCloseReason) override;

private:
    CryptoComData* owner_;
};

class CryptoComData final : public exec::DataClient {
public:
    explicit CryptoComData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_market}},
                handler_,
                net::WsPoolConfig{.max_keys_per_connection = ep::k_max_channels_per_connection,
                                  .send_spacing_ns = k_subscribe_spacing_ns,
                                  .max_connections = k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { invalidate_books(keys); }} {}

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
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        if (symbol.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const bool perp = instrument.market() == MarketType::Perp;
        switch (kind) {
        case exec::SubKind::Bbo:
            state_->wants_bbo.insert(instrument.packed);
            return open_book(symbol);
        case exec::SubKind::Depth:
            state_->wants_depth.insert(instrument.packed);
            return open_book(symbol);
        case exec::SubKind::Trades:
            return open(k_trade_prefix, symbol);
        case exec::SubKind::Funding:
            // Turnover has no SubKind; it rides Funding, and is all a pair gets.
            if (const auto ticker = open(k_ticker_prefix, symbol); !ticker.has_value()) {
                return ticker;
            }
            if (!perp) {
                return {};
            }
            for (const auto prefix : {k_funding_prefix, k_estimated_prefix, k_mark_prefix}) {
                if (const auto opened = open(prefix, symbol); !opened.has_value()) {
                    return opened;
                }
            }
            return {};
        case exec::SubKind::MarkPrice:
            if (!perp) {
                return DataClient::subscribe(kind, instrument);
            }
            return open(k_mark_prefix, symbol);
        case exec::SubKind::Kline:
            break;
        }
        return DataClient::subscribe(kind, instrument);
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

    void handle_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_event(frame);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Heartbeat:
            // On the socket that asked, with its id: unanswered for 5 s closes it.
            if (!pool_.reply(state_->protocol.heartbeat_reply(event->id))) {
                ++heartbeats_unanswered_;
            }
            break;
        case EventKind::Book:
        case EventKind::BookUpdate:
            handle_book(*event, received);
            break;
        case EventKind::Trade:
            handle_trades(*event, received);
            break;
        case EventKind::Ticker:
            handle_ticker(*event, received);
            break;
        case EventKind::Funding:
        case EventKind::EstimatedFunding:
        case EventKind::Mark:
            handle_funding(*event, received);
            break;
        case EventKind::Error:
            ++state_->subscribe_refusals;
            state_->last_error_code = event->code;
            if (first_refusal_.empty()) {
                first_refusal_.assign(event->message);
                if (!event->channel.empty()) {
                    first_refusal_.append(" — ").append(event->channel);
                }
            }
            break;
        case EventKind::Ack:
        case EventKind::Unknown:
            break;
        }
    }

    [[nodiscard]] VenueState& state() noexcept { return *state_; }

private:
    [[nodiscard]] static std::unexpected<exec::VenueError> pool_full() {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "cryptocom: connection pool is full")};
    }

    [[nodiscard]] std::expected<void, exec::VenueError> open_book(std::string_view symbol) {
        std::array<char, k_max_channel_len> buf{};
        return place(book_channel(symbol, buf));
    }

    [[nodiscard]] std::expected<void, exec::VenueError> open(std::string_view prefix,
                                                             std::string_view symbol) {
        std::array<char, k_max_channel_len> buf{};
        return place(channel_name(prefix, symbol, buf));
    }

    [[nodiscard]] std::expected<void, exec::VenueError> place(std::string_view channel) {
        if (channel.empty()) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "channel name too long")};
        }
        const auto [at, inserted] = state_->desired.emplace(channel);
        if (!inserted) {
            return {};
        }
        const std::array<std::string, 1> one{std::string{channel}};
        if (!pool_.subscribe(one)) {
            state_->desired.erase(at);
            return pool_full();
        }
        return {};
    }

    void handle_book(const Event& event, UnixNanos received) {
        const auto* listed = state_->instruments.find(event.instrument);
        if (listed == nullptr) {
            return;
        }
        const auto& base = base_of(*listed);
        static thread_local LevelBuffer levels;
        const auto wire = parse_book(event.data, event.kind == EventKind::Book, levels);
        if (!wire.has_value()) {
            return;
        }
        auto& book =
            state_->books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
                .first->second;
        switch (book.apply(*wire)) {
        case BookResult::Gap:
            resubscribe_book(base);
            return;
        case BookResult::BadLevel:
            // Not resubscribed: the next snapshot would carry the same level.
            ++bad_levels_;
            return;
        case BookResult::Ignored:
            return;
        case BookResult::Applied:
        case BookResult::Resynced:
            break;
        }
        const auto ts_event = ms_to_unix(wire->ts_ms, received);
        if (state_->wants_bbo.contains(base.id.packed)) {
            if (const auto bbo = book.bbo(ts_event, received); bbo.has_value()) {
                state_->sink->on_bbo(*bbo);
            }
        }
        if (state_->wants_depth.contains(base.id.packed) &&
            state_->sink->wants_depth(base.id, received)) {
            if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
                state_->sink->on_depth(*depth);
            }
        }
    }

    // Sent on the socket being handled; a repeat subscribe returns a fresh
    // snapshot and does not count against the 400.
    void resubscribe_book(const InstrumentBase& base) {
        std::array<char, k_max_channel_len> buf{};
        const auto channel = book_channel(base.exchange_symbol.view(), buf);
        if (channel.empty()) {
            return;
        }
        const std::array<std::string_view, 1> one{channel};
        const auto frame = state_->protocol.subscribe_frame(one);
        if (frame.has_value() && pool_.reply(frame->text)) {
            ++state_->book_resubscribes;
        }
    }

    void handle_trades(const Event& event, UnixNanos received) {
        // The subscribe answer replays the last 50 prints; they are not new flow.
        if (!event.is_push()) {
            ++state_->trade_replays_dropped;
            return;
        }
        const auto* listed = state_->instruments.find(event.instrument);
        if (listed == nullptr) {
            return;
        }
        static thread_local TradeBuffer trades;
        const auto wire = parse_trades(event.data, trades);
        if (!wire.has_value()) {
            return;
        }
        const auto& base = base_of(*listed);
        for (const auto& row : *wire) {
            if (const auto trade = map_trade(row, base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
            }
        }
    }

    void handle_ticker(const Event& event, UnixNanos received) {
        const auto* listed = state_->instruments.find(event.instrument);
        if (listed == nullptr) {
            return;
        }
        const auto wire = parse_ticker(event.data);
        if (!wire.has_value()) {
            return;
        }
        if (const auto stats = map_ticker_stats(*wire, base_of(*listed), received);
            stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
        }
    }

    // Unlike trades, a funding channel's subscribe answer is the current value.
    void handle_funding(const Event& event, UnixNanos received) {
        const auto* listed = state_->instruments.find(event.instrument);
        if (listed == nullptr) {
            return;
        }
        const auto wire = parse_value(event.data);
        if (!wire.has_value()) {
            return;
        }
        const auto& base = base_of(*listed);
        auto& memo = state_->funding[base.id.packed];
        std::optional<FundingUpdate> update;
        switch (event.kind) {
        case EventKind::Funding:
            update = map_funding_rate(*wire, base, memo, received);
            break;
        case EventKind::EstimatedFunding:
            update = map_estimated_rate(*wire, base, memo, received);
            break;
        default:
            update = map_mark(*wire, base, memo, received);
            break;
        }
        if (update.has_value()) {
            state_->sink->on_funding(*update);
        }
    }

    void invalidate_books(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (!key.starts_with(k_book_prefix)) {
                continue;
            }
            const auto* listed = state_->instruments.find(instrument_of(key));
            if (listed == nullptr) {
                continue;
            }
            if (const auto entry = state_->books.find(base_of(*listed).id.packed);
                entry != state_->books.end()) {
                entry->second.invalidate();
            }
        }
    }

    // Books first: they carry the quote.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> channels{keys.begin(), keys.end()};
        std::ranges::stable_partition(
            channels, [](std::string_view channel) { return channel.starts_with(k_book_prefix); });
        std::vector<std::string> frames;
        std::size_t built = 0;
        while (built < channels.size()) {
            const auto frame = state_->protocol.subscribe_frame(
                std::span<const std::string_view>{channels}.subspan(built));
            if (!frame.has_value() || frame->consumed == 0) {
                break;
            }
            frames.emplace_back(frame->text);
            built += frame->consumed;
        }
        return frames;
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        if (!state_->books.empty()) {
            const auto cold = std::ranges::count_if(
                state_->books, [](const auto& entry) { return !entry.second.synced(); });
            std::uint64_t gaps = 0;
            for (const auto& [packed, book] : state_->books) {
                gaps += book.gaps();
            }
            RLOG_INFO("cryptocom: {} books, {} unsynced at stop — {} chain gap(s), {} "
                      "resubscribe(s), {} refused for an off-step level",
                      state_->books.size(),
                      cold,
                      gaps,
                      state_->book_resubscribes,
                      bad_levels_);
        }
        if (state_->subscribe_refusals != 0) {
            RLOG_WARN("cryptocom: {} subscription(s) refused, first: {} (code {})",
                      state_->subscribe_refusals,
                      first_refusal_,
                      state_->last_error_code);
        }
        if (heartbeats_unanswered_ != 0) {
            RLOG_WARN("cryptocom: {} heartbeat(s) could not be answered — the venue closes a "
                      "socket that stays silent for five seconds",
                      heartbeats_unanswered_);
        }
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    std::string first_refusal_;
    std::uint64_t bad_levels_{0};
    std::uint64_t heartbeats_unanswered_{0};
    bool started_{false};
};

void PublicHandler::on_open() {
    auto& state = owner_->state();
    state.sink->on_connectivity(
        {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state.clock->wall()});
}

void PublicHandler::on_message(std::string_view payload, UnixNanos received) noexcept {
    owner_->handle_frame(payload, received);
}

void PublicHandler::on_close(net::WsCloseReason) {
    auto& state = owner_->state();
    state.sink->on_connectivity(
        {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state.clock->wall()});
}

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<CryptoComData>(std::move(state));
}

} // namespace raikiri::venues::cryptocom::detail
