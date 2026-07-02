#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/apex/book.hpp>
#include <raikiri/venues/apex/endpoints.hpp>
#include <raikiri/venues/apex/map.hpp>
#include <raikiri/venues/apex/wire.hpp>
#include <raikiri/venues/apex/ws.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::apex::detail {

namespace {

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

class ApexData final : public exec::DataClient {
public:
    explicit ApexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::stream_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_public}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { on_socket_open(keys); }} {}

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
        std::array<char, 64> buf{};
        const auto topic = topic_name(kind, symbol, buf);
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (kind == exec::SubKind::Depth) {
            state_->depth_wanted.insert(instrument.packed);
        }
        std::string key{topic};
        if (!state_->topics.insert(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            state_->topics.erase(one[0]);
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "apex: connection pool is full")};
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
    }

private:
    class Handler final : public net::WsHandler {
    public:
        explicit Handler(ApexData& owner) : owner_{&owner} {}

        void on_open() override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Resubscribed,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner_->on_frame(payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner_->state_->sink->on_connectivity({.state = exec::ConnState::Disconnected,
                                                   .stream_id = 0,
                                                   .ts = owner_->state_->clock->wall()});
        }

    private:
        ApexData* owner_;
    };

    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto topic = classify_topic(key);
            if (topic.kind != TopicKind::Orderbook) {
                continue;
            }
            const auto* instrument = state_->instruments.find(topic.symbol);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto book = state_->books.find(base_of(*instrument).id.packed);
                book != state_->books.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto topic = parse_topic(frame);
        if (!topic.has_value()) {
            return;
        }
        switch (topic->kind) {
        case TopicKind::Orderbook:
            on_book(frame, received);
            break;
        case TopicKind::Trade:
            on_trades(frame, received);
            break;
        case TopicKind::Instrument:
            on_ticker(frame, received);
            break;
        case TopicKind::Ping:
            // Unanswered, the server drops the socket after 165 s with no close frame.
            if (const auto value = parse_ping(frame); value.has_value()) {
                if (const auto pong = state_->protocol.pong_frame(*value); pong.has_value()) {
                    (void)pool_.reply(*pong);
                }
            }
            break;
        case TopicKind::Ack:
            on_ack(frame);
            break;
        case TopicKind::Unknown:
            break;
        }
    }

    // A refused frame subscribed none of its topics, including the valid ones.
    // Each is asked again on its own, once, so the refusal names its culprit.
    void on_ack(std::string_view frame) noexcept {
        static thread_local AckBuffer storage;
        const auto ack = parse_ack(frame, storage);
        if (!ack.has_value() || ack->success || ack->op != "subscribe") {
            return;
        }
        std::vector<std::string> topics{ack->args.begin(), ack->args.end()};
        const std::string reason{ack->ret_msg};
        const bool held = reason.find("already subscribed") != std::string::npos;
        if (topics.size() == 1) {
            if (held) {
                return;
            }
            ++state_->subscribe_refusals;
            if (has_logger()) {
                RLOG_WARN("apex: {} refused: {}", topics.front(), reason);
            }
            return;
        }
        for (const auto& topic : topics) {
            const std::array<std::string_view, 1> one{topic};
            if (const auto again = state_->protocol.subscribe_frame(one); again.has_value()) {
                (void)pool_.reply(*again);
            }
        }
    }

    void on_book(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<OrderbookBuffer>();
        const auto wire = parse_orderbook(frame, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state_->instruments.find(wire->symbol);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book =
            state_->books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
                .first->second;
        const auto result = book.apply(*wire);
        if (result == SyncResult::GapDetected) {
            resubscribe_book(wire->symbol);
            return;
        }
        if (result != SyncResult::Applied && result != SyncResult::Resynced) {
            return;
        }
        const auto ts_event = us_to_unix(wire->ts_us, received);
        if (const auto bbo = book.bbo(ts_event, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            state_->quoted.insert(base.id.packed);
        }
        if (!state_->depth_wanted.contains(base.id.packed) ||
            !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // Only a subscription produces a snapshot, so a broken chain is asked for
    // again on the socket that carried it.
    void resubscribe_book(std::string_view symbol) noexcept {
        std::array<char, 64> buf{};
        const std::array<std::string_view, 1> topic{topic_name(exec::SubKind::Depth, symbol, buf)};
        const auto off = state_->protocol.unsubscribe_frame(topic);
        if (!off.has_value() || !pool_.reply(*off)) {
            return;
        }
        const auto on = state_->protocol.subscribe_frame(topic);
        if (on.has_value() && pool_.reply(*on)) {
            ++state_->resubscribes;
        }
    }

    void on_trades(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<TradeBuffer>();
        const auto wire = parse_trades(frame, *storage);
        // The snapshot on subscribe replays the last fifty prints, some long past.
        if (!wire.has_value() || wire->is_snapshot) {
            return;
        }
        for (const auto& print : wire->trades) {
            const auto* instrument = state_->instruments.find(print.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (const auto trade = map_trade(print, base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                state_->traded.insert(base.id.packed);
            }
        }
    }

    void on_ticker(std::string_view frame, UnixNanos received) noexcept {
        const auto wire = parse_ticker(frame);
        if (!wire.has_value()) {
            return;
        }
        const auto* instrument = state_->instruments.find(wire->symbol);
        const auto* perp = instrument == nullptr ? nullptr : std::get_if<PerpInstrument>(instrument);
        if (perp == nullptr) {
            return;
        }
        auto& memo = state_->funding[perp->base.id.packed];
        if (const auto funding = map_funding(*wire, *perp, memo, received); funding.has_value()) {
            state_->sink->on_funding(*funding);
            if (!funding->rate.is_undef()) {
                state_->funded.insert(perp->base.id.packed);
            }
        }
        if (const auto stats = map_ticker_stats(*wire, perp->base, received); stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
        }
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> symbols;
        std::unordered_map<std::string_view, std::vector<std::string_view>> by_symbol;
        for (const auto& key : keys) {
            const auto symbol = classify_topic(key).symbol;
            auto& topics = by_symbol[symbol];
            if (topics.empty()) {
                symbols.push_back(symbol);
            }
            topics.push_back(key);
        }
        std::vector<std::string> frames;
        frames.reserve(symbols.size());
        for (const auto symbol : symbols) {
            const auto& topics = by_symbol[symbol];
            for (std::size_t at = 0; at < topics.size(); at += k_max_topics_per_frame) {
                const auto count = std::min(k_max_topics_per_frame, topics.size() - at);
                const auto frame = state_->protocol.subscribe_frame(
                    std::span<const std::string_view>{topics}.subspan(at, count));
                if (frame.has_value()) {
                    frames.emplace_back(*frame);
                }
            }
        }
        return frames;
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::uint64_t gaps = 0;
        std::size_t dark = 0;
        for (const auto& [id, book] : state_->books) {
            gaps += book.gaps();
            if (!book.synced()) {
                ++dark;
            }
        }
        RLOG_INFO("apex: {} of {} instruments quoted ({} books unsynced at stop), {} traded, {} "
                  "funded — {} chain gap(s), {} resubscribe(s), {} refused subscribe(s)",
                  state_->quoted.size(),
                  state_->instruments.size(),
                  dark,
                  state_->traded.size(),
                  state_->funded.size(),
                  gaps,
                  state_->resubscribes,
                  state_->subscribe_refusals);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ApexData>(std::move(state));
}

} // namespace raikiri::venues::apex::detail
