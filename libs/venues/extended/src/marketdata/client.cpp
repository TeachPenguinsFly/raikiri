#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/extended/book.hpp>
#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/venues/extended/endpoints.hpp>
#include <raikiri/venues/extended/map.hpp>
#include <raikiri/venues/extended/market_data.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include "../clients.hpp"

namespace raikiri::venues::extended::internal {

namespace {

constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr std::uint64_t k_poll_interval_ns = 300ULL * 1'000'000'000ULL;
// The catalogue is cached for 60 s, so a poll right after a settlement can
// still state the settlement just past.
constexpr std::uint64_t k_after_settlement_ns = 75ULL * 1'000'000'000ULL;
constexpr std::uint32_t k_poll_timeout_ms = 15'000;

using ep::Stream;

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

[[nodiscard]] std::string_view name_of(Stream stream) noexcept {
    constexpr std::array<std::string_view, ep::k_stream_count> names{
        "orderbooks", "orderbooks?depth=1", "publicTrades", "funding", "prices/mark"};
    return names[static_cast<std::size_t>(stream)];
}

class ExtendedData final : public exec::DataClient {
public:
    explicit ExtendedData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::host(state_->testnet)},
                                .port = "443",
                                .pool_size = 1,
                                .timeout_ms = k_poll_timeout_ms}} {
        for (std::size_t i = 0; i < ep::k_stream_count; ++i) {
            sockets_[i] = std::make_unique<Socket>(*this, static_cast<Stream>(i));
        }
        auto* owner = state_.get();
        rest_.set_admit(exec::market_data_gate(owner->budget, *owner->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        started_ = true;
        rest_.start();
        for (const auto& socket : sockets_) {
            if (socket->wanted) {
                socket->open();
            }
        }
        return {};
    }

    void stop() override {
        report_final();
        for (const auto& socket : sockets_) {
            socket->close();
        }
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return std::ranges::all_of(sockets_, [](const auto& socket) {
            return !socket->wanted || socket->client.connected();
        }) && std::ranges::any_of(sockets_, [](const auto& socket) { return socket->wanted; });
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        auto* market = state_->market_of(instrument);
        if (market == nullptr || state_->instruments.find(instrument) == nullptr) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        switch (kind) {
        case exec::SubKind::Bbo:
            market->quote = true;
            want(Stream::Book);
            want(Stream::Top);
            return {};
        case exec::SubKind::Depth:
            market->depth = true;
            want(Stream::Book);
            return {};
        case exec::SubKind::Trades:
            market->trades = true;
            want(Stream::Trades);
            return {};
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = true;
            polling_ = true;
            want(Stream::Funding);
            want(Stream::Mark);
            return {};
        case exec::SubKind::Kline:
            break;
        }
        return DataClient::subscribe(kind, instrument);
    }

    std::expected<void, exec::VenueError> unsubscribe(exec::SubKind kind,
                                                      InstrumentId instrument) override {
        auto* market = state_->market_of(instrument);
        if (market == nullptr) {
            return {};
        }
        switch (kind) {
        case exec::SubKind::Bbo:
            market->quote = false;
            break;
        case exec::SubKind::Depth:
            market->depth = false;
            break;
        case exec::SubKind::Trades:
            market->trades = false;
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = false;
            break;
        case exec::SubKind::Kline:
            break;
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
        for (const auto& socket : sockets_) {
            if (socket->restart) {
                socket->restart = false;
                socket->client.stop();
                socket->client.start();
                ++reconnects_;
            }
        }
        poll_markets(now);
    }

private:
    struct Socket final : net::WsHandler {
        Socket(ExtendedData& owner_ref, Stream stream_id)
            : owner{&owner_ref}, stream{stream_id},
              client{owner_ref.state_->io,
                     net::WsConfig{.host = std::string{ep::host(owner_ref.state_->testnet)},
                                   .port = "443",
                                   .target = std::string{ep::target(stream_id)}},
                     *this} {}

        void open() {
            if (!running) {
                running = true;
                client.start();
            }
        }

        void close() {
            if (running) {
                running = false;
                restart = false;
                client.stop();
            }
        }

        // A frame out of order means frames were lost, and the venue's answer
        // is to reconnect: nothing else resends them.
        [[nodiscard]] bool in_order(std::uint64_t seq) noexcept {
            if (restart) {
                return false;
            }
            if (last_seq != 0 && seq != last_seq + 1) {
                ++gaps;
                restart = true;
                if (has_logger()) {
                    RLOG_WARN("extended: {} jumped from seq {} to {}, reconnecting",
                              name_of(stream),
                              last_seq,
                              seq);
                }
                return false;
            }
            last_seq = seq;
            return true;
        }

        void on_open() override {
            last_seq = 0;
            opened_ms = owner->state_->clock->wall().ns / k_ns_per_ms;
            owner->state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                                  .stream_id = static_cast<std::uint32_t>(stream),
                                                  .ts = owner->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner->on_frame(*this, payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner->on_socket_closed(stream);
            owner->state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                                  .stream_id = static_cast<std::uint32_t>(stream),
                                                  .ts = owner->state_->clock->wall()});
        }

        ExtendedData* owner;
        Stream stream;
        net::WsClient client;
        bool wanted{false};
        bool running{false};
        bool restart{false};
        std::uint64_t last_seq{0};
        std::uint64_t opened_ms{0};
        std::uint64_t gaps{0};
    };

    void want(Stream stream) {
        auto& socket = *sockets_[static_cast<std::size_t>(stream)];
        socket.wanted = true;
        if (started_) {
            socket.open();
        }
    }

    void on_socket_closed(Stream stream) noexcept {
        if (stream == Stream::Book) {
            for (auto& [index, book] : state_->books) {
                book.invalidate();
            }
        } else if (stream == Stream::Top) {
            for (auto& market : state_->markets) {
                market.fast_quote = false;
            }
        }
    }

    struct Resolved {
        const InstrumentBase* base{nullptr};
        Market* market{nullptr};
    };

    // Streams carry delisted markets and a few the catalogue never listed;
    // they resolve to nothing and are dropped without a count.
    [[nodiscard]] Resolved resolve(std::string_view name) noexcept {
        const auto* instrument = state_->instruments.find(name);
        if (instrument == nullptr) {
            return {};
        }
        const auto& base = base_of(*instrument);
        return {.base = &base, .market = state_->market_of(base.id)};
    }

    void on_frame(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        switch (socket.stream) {
        case Stream::Book:
            on_book(socket, frame, received);
            break;
        case Stream::Top:
            on_top(socket, frame, received);
            break;
        case Stream::Trades:
            on_trades(socket, frame, received);
            break;
        case Stream::Funding:
            on_funding(socket, frame, received);
            break;
        case Stream::Mark:
            on_mark(socket, frame, received);
            break;
        }
    }

    void on_book(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_book(frame, *levels_);
        if (!wire.has_value() || !socket.in_order(wire->seq)) {
            return;
        }
        const auto [base, market] = resolve(wire->market);
        if (market == nullptr || (!market->quote && !market->depth)) {
            return;
        }
        auto& book = state_->books.try_emplace(base->id.symbol().raw, base->id, base->tick_size,
                                               base->lot_size)
                         .first->second;
        const auto result = book.apply(*wire);
        if (result == BookResult::BadLevel) {
            ++off_grid_;
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        const auto ts_event = ms_to_unix(wire->ts_ms, received);
        if (market->quote && !market->fast_quote) {
            if (const auto top = book.top(ts_event, received); top.has_value()) {
                state_->sink->on_bbo(*top);
                market->priced = true;
            }
        }
        if (!market->depth || !state_->sink->wants_depth(base->id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    void on_top(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_book(frame, *levels_);
        if (!wire.has_value() || !socket.in_order(wire->seq)) {
            return;
        }
        const auto [base, market] = resolve(wire->market);
        if (market == nullptr || !market->quote) {
            return;
        }
        if (const auto top = map_top(*wire, base->id, base->tick_size, base->lot_size, received);
            top.has_value()) {
            state_->sink->on_bbo(*top);
            market->fast_quote = true;
            market->priced = true;
        }
    }

    // The first frame on every connection replays the last fifty prints. Ids
    // rise per market, so a print at or below the last one seen is a replay;
    // on a market not yet seen, one older than the connection is.
    void on_trades(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        static thread_local PrintBuffer storage;
        const auto wire = parse_trades(frame, storage);
        if (!wire.has_value() || !socket.in_order(wire->seq)) {
            return;
        }
        for (const auto& print : wire->prints) {
            const auto [base, market] = resolve(print.market);
            if (market == nullptr || !market->trades || print.id <= market->last_trade_id) {
                continue;
            }
            const bool replay = market->last_trade_id == 0 && print.ts_ms < socket.opened_ms;
            market->last_trade_id = print.id;
            if (replay) {
                continue;
            }
            if (const auto trade = map_trade(print, *base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                market->traded = true;
            }
        }
    }

    void on_funding(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_funding(frame);
        if (!wire.has_value() || !socket.in_order(wire->seq)) {
            return;
        }
        const auto [base, market] = resolve(wire->market);
        if (market == nullptr || !market->funding || !note_rate(*wire, market->memo)) {
            return;
        }
        state_->sink->on_funding(
            merged(*base, market->memo, ms_to_unix(wire->ts_ms, received), received));
        market->funded = true;
    }

    void on_mark(Socket& socket, std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_mark(frame);
        if (!wire.has_value() || !socket.in_order(wire->seq)) {
            return;
        }
        const auto [base, market] = resolve(wire->market);
        if (market == nullptr || !market->funding || !note_mark(*wire, *base, market->memo)) {
            return;
        }
        state_->sink->on_funding(
            merged(*base, market->memo, ms_to_unix(wire->ts_ms, received), received));
    }

    // The catalogue again, for what no stream carries: the next settlement
    // time and the day's turnover.
    void poll_markets(UnixNanos now) noexcept {
        if (!polling_ || poll_in_flight_ || now.ns < next_poll_ns_) {
            return;
        }
        poll_in_flight_ = true;
        const net::RestRequest request{
            .method = net::HttpMethod::Get, .target = ep::k_markets, .body = {}};
        try {
            if (!rest_.send(request, [this](net::RestResponse&& response) {
                    on_markets(response);
                })) {
                poll_in_flight_ = false;
                return;
            }
        } catch (...) {
            poll_in_flight_ = false;
            return;
        }
        next_poll_ns_ = now.ns + k_poll_interval_ns;
    }

    void on_markets(const net::RestResponse& response) noexcept {
        poll_in_flight_ = false;
        if (response.transport_failed() || response.status != 200) {
            if (response.status == 429 && state_->budget != nullptr) {
                state_->budget->penalize_all(state_->clock->mono(), 60ULL * 1'000'000'000ULL);
            }
            return;
        }
        static thread_local const auto storage = std::make_unique<MarketBuffer>();
        const auto rows = parse_markets(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        UnixNanos next_settlement{};
        for (const auto& row : *rows) {
            const auto [base, market] = resolve(row.name);
            if (market == nullptr) {
                continue;
            }
            note_market(row, *base, market->memo);
            if (market->memo.next_funding > received &&
                (next_settlement.ns == 0 || market->memo.next_funding < next_settlement)) {
                next_settlement = market->memo.next_funding;
            }
            if (!market->funding) {
                continue;
            }
            state_->sink->on_funding(merged(*base, market->memo, received, received));
            if (const auto stats = map_ticker_stats(row, *base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
        if (next_settlement.ns != 0) {
            next_poll_ns_ = std::min(next_poll_ns_, next_settlement.ns + k_after_settlement_ns);
        }
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::size_t synced = 0;
        for (const auto& [index, book] : state_->books) {
            synced += book.synced() ? 1U : 0U;
        }
        std::size_t priced = 0;
        std::size_t priced_rfq = 0;
        std::size_t traded = 0;
        std::size_t funded = 0;
        for (const auto& market : state_->markets) {
            traded += market.traded ? 1 : 0;
            funded += market.funded ? 1 : 0;
            if (!market.priced) {
                continue;
            }
            ++priced;
            priced_rfq += market.rfq ? 1 : 0;
        }
        std::uint64_t gaps = 0;
        for (const auto& socket : sockets_) {
            gaps += socket->gaps;
        }
        RLOG_INFO("extended: {} of {} markets priced ({} RFQ), {} books synced of {}, {} traded, "
                  "{} funded — {} seq gap(s), {} reconnect(s), {} off-grid book(s)",
                  priced,
                  state_->markets.size(),
                  priced_rfq,
                  synced,
                  state_->books.size(),
                  traded,
                  funded,
                  gaps,
                  reconnects_,
                  off_grid_);
    }

    std::shared_ptr<VenueState> state_;
    net::RestClient rest_;
    std::array<std::unique_ptr<Socket>, ep::k_stream_count> sockets_;
    std::unique_ptr<LevelBuffer> levels_{std::make_unique<LevelBuffer>()};
    bool started_{false};
    bool polling_{false};
    bool poll_in_flight_{false};
    std::uint64_t next_poll_ns_{0};
    std::uint64_t reconnects_{0};
    std::uint64_t off_grid_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ExtendedData>(std::move(state));
}

} // namespace raikiri::venues::extended::internal
