#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/paradex/endpoints.hpp>
#include <raikiri/venues/paradex/map.hpp>
#include <raikiri/venues/paradex/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::paradex::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;

constexpr std::string_view k_book_prefix = "order_book.";
constexpr std::string_view k_book_suffix = ".deltas";
constexpr std::string_view k_funding_channel = "funding_data.ALL";

[[nodiscard]] std::string channel_for(exec::SubKind kind, std::string_view market) {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        return std::string{k_book_prefix} + std::string{market} + std::string{k_book_suffix};
    case exec::SubKind::Trades:
        return "trades." + std::string{market};
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "markets_summary." + std::string{market};
    case exec::SubKind::Kline:
        break;
    }
    return {};
}

[[nodiscard]] std::string frame_for(std::string_view method, std::string_view channel) {
    return R"({"jsonrpc":"2.0","method":")" + std::string{method} + R"(","params":{"channel":")" +
           std::string{channel} + R"("},"id":1})";
}

[[nodiscard]] std::string_view book_market(std::string_view key) noexcept {
    if (!key.starts_with(k_book_prefix) || !key.ends_with(k_book_suffix)) {
        return {};
    }
    key.remove_prefix(k_book_prefix.size());
    key.remove_suffix(k_book_suffix.size());
    return key;
}

class ParadexData final : public exec::DataClient, public net::WsHandler {
public:
    explicit ParadexData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, pool_{state_->io,
                                          net::WsConfig{.host = std::string{ep::ws_host(state_->testnet)},
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
        auto channel = channel_for(kind, symbol);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        std::vector<std::string> keys;
        if (kind == exec::SubKind::Funding || kind == exec::SubKind::MarkPrice) {
            if (market->funding) {
                return {};
            }
            market->funding = true;
            if (!state_->funding_asked) {
                state_->funding_asked = true;
                keys.emplace_back(k_funding_channel);
            }
        }
        if (kind == exec::SubKind::Bbo || kind == exec::SubKind::Depth) {
            if (market->depth) {
                return {};
            }
            market->depth = true;
        }
        keys.push_back(std::move(channel));
        if (!pool_.subscribe(keys)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "paradex: connection pool is full")};
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
        if (!is_sbe(payload)) {
            on_reply(payload);
            return;
        }
        switch (template_of(payload)) {
        case Template::Book:
            on_book(payload, received);
            break;
        case Template::Trade:
            on_trade(payload, received);
            break;
        case Template::Summary:
            on_summary(payload, received);
            break;
        case Template::Funding:
            on_funding(payload, received);
            break;
        case Template::Bbo:
        case Template::Unknown:
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

    void on_reply(std::string_view payload) {
        const auto reply = parse_reply(payload);
        if (reply.has_value() && reply->code != 0) {
            ++state_->subscribe_refusals;
        }
    }

    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_book(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->market);
        if (!found.has_value()) {
            return;
        }
        auto& market = *found->market;
        if (!market.book.has_value()) {
            market.book.emplace(*found->base);
        }
        auto& book = *market.book;
        const auto result = book.on_frame(*wire);
        if (book.take_resync(received.ns)) {
            resubscribe_book(wire->market);
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        const auto stamp = us_to_unix(wire->ts_us);
        if (const auto top = book.top(stamp, received); top.has_value()) {
            state_->sink->on_bbo(*top);
            market.priced = true;
        }
        if (!state_->sink->wants_depth(found->base->id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(stamp, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // on the socket being dispatched, which is the one carrying this book; its first frame is a snapshot
    void resubscribe_book(std::string_view market) noexcept {
        const auto channel = std::string{k_book_prefix} + std::string{market} + std::string{k_book_suffix};
        if (!pool_.reply(frame_for("unsubscribe", channel))) {
            return;
        }
        if (pool_.reply(frame_for("subscribe", channel))) {
            ++state_->resubscribes;
        }
    }

    void on_trade(std::string_view payload, UnixNanos received) {
        const auto wire = parse_trade(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->market);
        if (!found.has_value() || wire->trade_id == found->market->last_trade_id) {
            return;
        }
        found->market->last_trade_id = wire->trade_id;
        if (const auto trade = map_trade(*wire, *found->base, received); trade.has_value()) {
            state_->sink->on_trade(*trade);
            found->market->traded = true;
        }
    }

    void on_summary(std::string_view payload, UnixNanos received) {
        const auto wire = parse_summary(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->market);
        if (!found.has_value()) {
            return;
        }
        found->market->mark = wire->mark;
        found->market->index = wire->index;
        if (const auto stats = map_ticker_stats(*wire, *found->base, received); stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
            found->market->summarised = true;
        }
    }

    void on_funding(std::string_view payload, UnixNanos received) {
        const auto wire = parse_funding(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->market);
        if (!found.has_value() || !found->market->funding) {
            return;
        }
        const auto funding = map_funding(*wire, *found->base, found->market->mark, found->market->index, received);
        if (funding.has_value()) {
            state_->sink->on_funding(*funding);
            found->market->funded = true;
        }
    }

    void on_connection_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            if (const auto found = lookup(book_market(key)); found.has_value() && found->market->book.has_value()) {
                found->market->book->invalidate();
            }
        }
    }

    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> frames;
        frames.reserve(keys.size());
        for (const auto& key : keys) {
            frames.push_back(frame_for("subscribe", key));
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
        std::size_t summarised = 0;
        std::uint64_t gaps = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
            summarised += market.summarised ? 1U : 0U;
            gaps += market.book.has_value() ? market.book->gaps() : 0U;
        }
        RLOG_INFO("paradex: {} of {} markets priced, {} traded, {} funded, {} with volume; {} gaps, {} resubscribes; "
                  "{} sockets, {} keys, {} refusals",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  summarised,
                  gaps,
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
    return std::make_unique<ParadexData>(std::move(state));
}

} // namespace raikiri::venues::paradex::internal
