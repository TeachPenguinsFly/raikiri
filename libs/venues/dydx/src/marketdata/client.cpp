#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/dydx/book.hpp>
#include <raikiri/venues/dydx/endpoints.hpp>
#include <raikiri/venues/dydx/map.hpp>
#include <raikiri/venues/dydx/wire.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::dydx::internal {

namespace {

constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

// The rate is a mean of per-minute samples, so a minute is its own cadence.
constexpr std::uint64_t k_catalogue_poll_ns = 60 * k_ns_per_s;
constexpr std::uint32_t k_poll_timeout_ms = 10'000;

// How long after the last subscribe goes out before the unanswered ones are named.
constexpr std::uint64_t k_coverage_grace_ns = 10 * k_ns_per_s;

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

class DydxData final : public exec::DataClient {
public:
    explicit DydxData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::indexer_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws}},
                handler_,
                // One key is one ticker and costs one subscription on each channel.
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { on_socket_open(keys); }},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::indexer_host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_poll_timeout_ms}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto ticker = state_->instruments.exchange_symbol(instrument);
        if (ticker.empty()) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        switch (kind) {
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            state_->funding_wanted.insert(instrument.packed);
            return {};
        case exec::SubKind::Depth:
            state_->depth_wanted.insert(instrument.packed);
            break;
        case exec::SubKind::Trades:
            state_->trades_wanted.insert(instrument.packed);
            break;
        case exec::SubKind::Bbo:
            break;
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        std::string key{ticker};
        if (!placed_.insert(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            placed_.erase(one[0]);
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "dydx: every socket the pool may open is full")};
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
        poll_catalogue(now);
        report_coverage(now);
    }

private:
    class Handler final : public net::WsHandler {
    public:
        explicit Handler(DydxData& owner) : owner_{&owner} {}

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
        DydxData* owner_;
    };

    // Both channels for every key: a Trades request that arrives after its
    // ticker's Bbo placed the key would otherwise have no frame to ride.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> frames;
        frames.reserve(keys.size() * 2);
        for (const auto channel : {Channel::Book, Channel::Trades}) {
            for (const auto& key : keys) {
                if (const auto frame = state_->protocol.subscribe(channel, key); !frame.empty()) {
                    frames.emplace_back(frame);
                }
            }
        }
        return frames;
    }

    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            state_->acked_books.erase(key);
            state_->acked_trades.erase(key);
            const auto* instrument = state_->instruments.find(std::string_view{key});
            if (instrument == nullptr) {
                continue;
            }
            if (const auto book = state_->books.find(base_of(*instrument).id.packed); book != state_->books.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<FrameBuffer>();
        const auto wire = parse_frame(frame, *storage);
        if (!wire.has_value()) {
            return;
        }
        switch (wire->type) {
        case FrameType::Subscribed:
            if (wire->channel == Channel::Book) {
                state_->acked_books.emplace(wire->id);
                on_book(*wire, received, true);
            } else if (wire->channel == Channel::Trades) {
                state_->acked_trades.emplace(wire->id); // its backlog is history, not prints
            }
            break;
        case FrameType::Data:
            if (wire->channel == Channel::Book) {
                on_book(*wire, received, false);
            } else if (wire->channel == Channel::Trades) {
                on_trades(*wire, received);
            }
            break;
        case FrameType::Error:
            on_refusal(*wire);
            break;
        case FrameType::Connected:
        case FrameType::Unsubscribed:
        case FrameType::Unknown:
            break;
        }
    }

    void on_book(const WireFrame& wire, UnixNanos received, bool snapshot) noexcept {
        const auto* instrument = state_->instruments.find(wire.id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = state_->books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first->second;
        BookResult result = BookResult::Lost;
        if (wire.overflow) {
            ++state_->overflows;
            book.drop();
        } else {
            result = snapshot ? book.reset(wire.bids, wire.asks) : book.apply(wire.bids, wire.asks);
        }
        if (book.take_resync(received.ns)) {
            resubscribe_book(wire.id);
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        if (const auto top = book.top(received, received); top.has_value()) {
            state_->sink->on_bbo(*top);
        }
        if (!state_->depth_wanted.contains(base.id.packed) || !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(received, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // On the socket being dispatched, which is the one carrying this book.
    void resubscribe_book(std::string_view ticker) noexcept {
        const auto off = state_->protocol.unsubscribe(Channel::Book, ticker);
        if (off.empty() || !pool_.reply(off)) {
            return;
        }
        const auto on = state_->protocol.subscribe(Channel::Book, ticker);
        if (!on.empty() && pool_.reply(on)) {
            ++state_->resubscribes;
        }
    }

    void on_trades(const WireFrame& wire, UnixNanos received) noexcept {
        const auto* instrument = state_->instruments.find(wire.id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        if (!state_->trades_wanted.contains(base.id.packed)) {
            return;
        }
        for (const auto& print : wire.prints) {
            if (const auto trade = map_trade(print, base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                state_->traded.insert(base.id.packed);
            }
        }
    }

    // A limit refusal names its channel in the text and no ticker; which key it
    // was is the one that never got `subscribed`, named by report_coverage.
    void on_refusal(const WireFrame& wire) noexcept {
        ++state_->subscribe_refusals;
        state_->last_refusal = std::string{wire.message};
        if (has_logger()) {
            RLOG_WARN("dydx: refused {} {} — {}",
                      channel_name(wire.channel != Channel::Other ? wire.channel : refused_channel(wire.message)),
                      wire.id,
                      wire.message);
        }
    }

    void poll_catalogue(UnixNanos now) noexcept {
        if (state_->funding_wanted.empty() || in_flight_ || now.ns < next_poll_ns_) {
            return;
        }
        in_flight_ = true;
        try {
            const net::RestRequest request{
                .method = net::HttpMethod::Get, .target = ep::k_perpetual_markets, .body = {}};
            if (!rest_.send(request, [this](net::RestResponse&& response) { on_catalogue(response); })) {
                in_flight_ = false;
                return;
            }
        } catch (...) {
            in_flight_ = false;
            return;
        }
        next_poll_ns_ = now.ns + k_catalogue_poll_ns;
    }

    void on_catalogue(const net::RestResponse& response) noexcept {
        in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            next_poll_ns_ = response.received.ns + ep::k_rest_window_ns;
            return;
        }
        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_perpetual_markets(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = response.received;
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.ticker);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (!state_->funding_wanted.contains(base.id.packed) || !still_active(row)) {
                continue;
            }
            if (const auto funding = map_funding(row, base, received); funding.has_value()) {
                state_->sink->on_funding(*funding);
                state_->funded.insert(base.id.packed);
            }
            if (const auto stats = map_ticker_stats(row, base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    [[nodiscard]] bool still_active(const WireMarket& row) noexcept {
        std::string ticker{row.ticker};
        if (row.status != "ACTIVE") {
            if (state_->halted.insert(ticker).second && has_logger()) {
                RLOG_WARN("dydx: {} is {} — its funding is no longer published", ticker, row.status);
            }
            return false;
        }
        if (state_->halted.erase(ticker) != 0 && has_logger()) {
            RLOG_INFO("dydx: {} is ACTIVE again", ticker);
        }
        return true;
    }

    [[nodiscard]] static std::string unanswered(const std::unordered_set<std::string>& placed,
                                                const std::unordered_set<std::string>& acked,
                                                std::size_t& count) {
        std::string names;
        count = 0;
        for (const auto& key : placed) {
            if (acked.contains(key)) {
                continue;
            }
            if (++count <= 8) {
                names += names.empty() ? key : " " + key;
            }
        }
        return names;
    }

    void report_coverage(UnixNanos now) noexcept {
        if (coverage_reported_ || placed_.empty()) {
            return;
        }
        if (pool_.pending_sends() != 0) {
            coverage_due_ns_ = 0;
            return;
        }
        if (coverage_due_ns_ == 0) {
            coverage_due_ns_ = now.ns + k_coverage_grace_ns;
            return;
        }
        if (now.ns < coverage_due_ns_) {
            return;
        }
        coverage_reported_ = true;
        log_coverage();
    }

    void log_coverage() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::size_t books = 0;
        std::size_t tapes = 0;
        const auto book_names = unanswered(placed_, state_->acked_books, books);
        const auto tape_names = unanswered(placed_, state_->acked_trades, tapes);
        if (books == 0 && tapes == 0) {
            RLOG_INFO("dydx: {} tickers on {} socket(s), every book and tape acknowledged",
                      placed_.size(),
                      pool_.connection_count());
            return;
        }
        RLOG_WARN("dydx: of {} tickers, {} book(s) [{}] and {} tape(s) [{}] never acknowledged",
                  placed_.size(),
                  books,
                  book_names,
                  tapes,
                  tape_names);
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::size_t synced = 0;
        std::size_t crossed = 0;
        std::uint64_t crossings = 0;
        std::uint64_t outside = 0;
        for (const auto& [packed, book] : state_->books) {
            synced += book.synced() ? 1U : 0U;
            crossed += book.crossed() ? 1U : 0U;
            crossings += book.crossings();
            outside += book.outside_band();
        }
        RLOG_INFO("dydx: {} of {} tickers booked ({} crossed at stop), {} traded, {} funded — "
                  "{} crossing(s), {} resubscribe(s), {} overflow(s), {} level(s) outside the band",
                  synced,
                  placed_.size(),
                  crossed,
                  state_->traded.size(),
                  state_->funded.size(),
                  crossings,
                  state_->resubscribes,
                  state_->overflows,
                  outside);
        if (!coverage_reported_) {
            log_coverage();
        }
        if (state_->subscribe_refusals != 0) {
            RLOG_WARN("dydx: the venue refused {} subscription(s), the last: {}",
                      state_->subscribe_refusals,
                      state_->last_refusal);
        }
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    std::unordered_set<std::string> placed_;
    bool started_{false};
    bool in_flight_{false};
    bool coverage_reported_{false};
    std::uint64_t next_poll_ns_{0};
    std::uint64_t coverage_due_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<DydxData>(std::move(state));
}

} // namespace raikiri::venues::dydx::internal
