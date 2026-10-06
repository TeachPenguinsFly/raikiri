#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/arcus/endpoints.hpp>
#include <raikiri/venues/arcus/map.hpp>
#include <raikiri/venues/arcus/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::arcus::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;

constexpr std::string_view k_markets_channel = "markets";
constexpr std::string_view k_book_channel = "l2OrderbookUpdates";
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bbo";
    case exec::SubKind::Depth:
        return k_book_channel;
    case exec::SubKind::Trades:
        return "trades";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return k_markets_channel;
    case exec::SubKind::Kline:
        break;
    }
    return {};
}

[[nodiscard]] std::string frame_for(std::string_view verb, std::string_view channel, std::string_view id) {
    std::string frame = R"({"type":")" + std::string{verb} + R"(","channel":")" + std::string{channel} + '"';
    if (!id.empty()) {
        frame += R"(,"id":")" + std::string{id} + '"';
    }
    if (channel == k_book_channel && verb == "subscribe") {
        frame += R"(,"nLevels":)" + std::to_string(ep::k_book_depth);
    }
    frame += '}';
    return frame;
}

[[nodiscard]] std::string frame_for_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return frame_for("subscribe", key, {});
    }
    return frame_for("subscribe", key.substr(0, cut), key.substr(cut + 1));
}

class ArcusData final : public exec::DataClient, public net::WsHandler {
public:
    explicit ArcusData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, pool_{state_->io,
                                          net::WsConfig{.host = std::string{ep::rest_host(state_->testnet)},
                                                        .port = "443",
                                                        .target = std::string{ep::k_ws_path}},
                                          *this,
                                          net::WsPoolConfig{},
                                          [](std::span<const std::string> keys) { return build_frames(keys); },
                                          [this](std::span<const std::string> keys) { on_connection_open(keys); }} {}

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        auto* market = state_->market_of(instrument);
        if (symbol.empty() || market == nullptr) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto channel = channel_for(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        std::string key{channel};
        if (channel == k_markets_channel) {
            market->funding = true;
            if (state_->stats_asked) {
                return {};
            }
            state_->stats_asked = true;
        } else {
            if (channel == k_book_channel) {
                market->depth = true;
            }
            key.push_back(k_key_sep);
            key.append(symbol);
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "arcus: connection pool is full")};
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
        if (now.ns >= state_->next_report_ns) {
            if (state_->next_report_ns != 0) {
                report();
            }
            state_->next_report_ns = now.ns + k_report_interval_ns;
        }
    }

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto head = parse_head(payload);
        if (!head.has_value()) {
            return;
        }
        switch (head->type) {
        case FrameType::Subscribed:
        case FrameType::Data:
            dispatch(*head, payload, received);
            break;
        case FrameType::Degraded:
            on_degraded(*head, received);
            break;
        case FrameType::Error:
            // an unsubscribe for a book that already lapsed is answered this way
            if (!head->message.starts_with("Not subscribed")) {
                ++state_->subscribe_refusals;
            }
            break;
        case FrameType::Connected:
        case FrameType::Unsubscribed:
        case FrameType::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    struct Found {
        const InstrumentBase* base;
        Market* market;
    };

    [[nodiscard]] std::optional<Found> lookup(std::string_view symbol) noexcept {
        const auto* instrument = state_->instruments.find(symbol);
        if (instrument == nullptr) {
            return std::nullopt;
        }
        const auto& base = base_of(*instrument);
        auto* market = state_->market_of(base.id);
        if (market == nullptr) {
            return std::nullopt;
        }
        return Found{&base, market};
    }

    void dispatch(const WireHead& head, std::string_view payload, UnixNanos received) {
        switch (head.channel) {
        case Channel::Bbo:
            on_bbo(payload, received);
            break;
        case Channel::Book:
            on_book(payload, received, head.type == FrameType::Subscribed);
            break;
        case Channel::Trades:
            if (head.type == FrameType::Data) {
                on_trades(payload, received);
            }
            break;
        case Channel::Markets:
            on_stats(payload, received);
            break;
        case Channel::Unknown:
            break;
        }
    }

    void on_bbo(std::string_view payload, UnixNanos received) {
        const auto wire = parse_bbo(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->id);
        if (!found.has_value() || wire->seq < found->market->last_quote_seq) {
            return;
        }
        found->market->last_quote_seq = wire->seq;
        if (const auto bbo = map_bbo(*wire, *found->base, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            found->market->priced = true;
        }
    }

    void on_book(std::string_view payload, UnixNanos received, bool snapshot) {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_book(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->id);
        if (!found.has_value()) {
            return;
        }
        auto& market = *found->market;
        if (!market.book.has_value()) {
            market.book.emplace(found->base->id, found->base->tick_size, found->base->lot_size);
        }
        auto& book = *market.book;
        const auto result = snapshot ? book.reset(*wire) : book.apply(*wire);
        if (book.take_resync(received.ns)) {
            resubscribe_book(wire->id);
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        if (!state_->sink->wants_depth(found->base->id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(UnixNanos{}, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    void on_degraded(const WireHead& head, UnixNanos received) {
        ++state_->degraded;
        if (head.channel != Channel::Book) {
            return;
        }
        const auto found = lookup(head.id);
        if (!found.has_value() || !found->market->book.has_value()) {
            return;
        }
        found->market->book->drop();
        if (found->market->book->take_resync(received.ns)) {
            resubscribe_book(head.id);
        }
    }

    // on the socket being dispatched, which is the one carrying this book
    void resubscribe_book(std::string_view id) noexcept {
        if (!pool_.reply(frame_for("unsubscribe", k_book_channel, id))) {
            return;
        }
        if (pool_.reply(frame_for("subscribe", k_book_channel, id))) {
            ++state_->resubscribes;
        }
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<PrintBuffer>();
        const auto rows = parse_trades(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.symbol);
            if (!found.has_value() || row.trade_id <= found->market->last_trade_id) {
                continue;
            }
            found->market->last_trade_id = row.trade_id;
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    void on_stats(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<StatsBuffer>();
        const auto rows = parse_stats(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.symbol);
            if (!found.has_value() || !found->market->funding) {
                continue;
            }
            if (const auto funding = map_funding(row, *found->base, received); funding.has_value()) {
                state_->sink->on_funding(*funding);
                found->market->funded = true;
            }
            if (const auto stats = map_ticker_stats(row, *found->base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    // only this socket's books resync; the subscribe brings each a fresh snapshot
    void on_connection_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            if (cut == std::string::npos || std::string_view{key}.substr(0, cut) != k_book_channel) {
                continue;
            }
            if (const auto found = lookup(std::string_view{key}.substr(cut + 1));
                found.has_value() && found->market->book.has_value()) {
                found->market->book->invalidate();
            }
        }
    }

    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> frames;
        frames.reserve(keys.size());
        for (const auto& key : keys) {
            frames.push_back(frame_for_key(key));
        }
        return frames;
    }

    void report() const {
        if (::raikiri::log::logger() == nullptr) {
            return;
        }
        std::size_t priced = 0;
        std::size_t traded = 0;
        std::size_t funded = 0;
        std::size_t booked = 0;
        std::uint64_t gaps = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
            booked += market.book.has_value() && market.book->synced() ? 1U : 0U;
            gaps += market.book.has_value() ? market.book->gaps() : 0U;
        }
        RLOG_INFO("arcus: {} of {} markets priced, {} traded, {} funded, {} books synced; {} gaps, {} degraded, "
                  "{} resubscribes; {} sockets, {} keys, {} refusals",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  booked,
                  gaps,
                  state_->degraded,
                  state_->resubscribes,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals);
    }

    std::shared_ptr<VenueState> state_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ArcusData>(std::move(state));
}

} // namespace raikiri::venues::arcus::internal
