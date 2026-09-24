#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/pacifica/endpoints.hpp>
#include <raikiri/venues/pacifica/map.hpp>
#include <raikiri/venues/pacifica/wire.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::pacifica::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;

constexpr std::string_view k_prices = "prices";
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string_view source_of(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bbo";
    case exec::SubKind::Depth:
        return "book";
    case exec::SubKind::Trades:
        return "trades";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return k_prices;
    case exec::SubKind::Kline:
        break;
    }
    return {};
}

[[nodiscard]] std::string frame_for(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return R"({"method":"subscribe","params":{"source":")" + std::string{key} + R"("}})";
    }
    const auto source = key.substr(0, cut);
    std::string frame = R"({"method":"subscribe","params":{"source":")" + std::string{source} +
                        R"(","symbol":")" + std::string{key.substr(cut + 1)} + '"';
    if (source == "book") {
        frame += R"(,"agg_level":1)";
    }
    frame += "}}";
    return frame;
}

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (auto& market : state_->markets) {
            market.quote = {};
            market.book = {};
        }
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_channel(payload)) {
        case Channel::Book:
            on_book(payload, received);
            break;
        case Channel::Bbo:
            on_bbo(payload, received);
            break;
        case Channel::Trades:
            on_trades(payload, received);
            break;
        case Channel::Prices:
            on_prices(payload, received);
            break;
        case Channel::Error:
            ++state_->subscribe_refusals;
            break;
        case Channel::Pong:
        case Channel::Subscribed:
        case Channel::Unknown:
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

    void publish_top(const WireBbo& wire, const Found& found, UnixNanos received) {
        if (!found.market->quote.admit(wire.li)) {
            return;
        }
        if (const auto bbo = map_bbo(wire, *found.base, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            found.market->priced = true;
        }
    }

    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value() || !found->market->book.admit(wire->li)) {
            return;
        }
        // bbo fires only when the top moves; the ladder's level zero keeps a quiet market quoted
        if (const auto top = top_of(*wire); top.has_value()) {
            publish_top(*top, *found, received);
        }
        if (const auto book = map_book(*wire, *found->base, received); book.has_value()) {
            state_->sink->on_depth(*book);
        }
    }

    void on_bbo(std::string_view payload, UnixNanos received) {
        const auto wire = parse_bbo(payload);
        if (!wire.has_value()) {
            return;
        }
        if (const auto found = lookup(wire->symbol); found.has_value()) {
            publish_top(*wire, *found, received);
        }
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local PrintBuffer storage;
        const auto rows = parse_trades(payload, storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.symbol);
            if (!found.has_value() || row.history_id <= found->market->last_history_id) {
                continue;
            }
            found->market->last_history_id = row.history_id;
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    void on_prices(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<PriceBuffer>();
        const auto rows = parse_prices(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.symbol);
            if (!found.has_value() || !found->market->funding) {
                continue;
            }
            if (auto funding = map_funding(row, *found->base, received); funding.has_value()) {
                found->market->roll.apply(*funding);
                state_->sink->on_funding(*funding);
                found->market->funded = true;
            }
            if (const auto stats = map_ticker_stats(row, *found->base, received);
                stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    VenueState* state_;
};

class PacificaData final : public exec::DataClient {
public:
    explicit PacificaData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::ws_host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }} {}

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

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override {
        return state_->subscribe_refusals;
    }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        auto* market = state_->market_of(instrument);
        if (symbol.empty() || market == nullptr) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto source = source_of(kind);
        if (source.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        std::string key{source};
        if (source == k_prices) {
            market->funding = true;
            if (state_->prices_asked) {
                return {};
            }
            state_->prices_asked = true;
        } else {
            key.push_back(k_key_sep);
            key.append(symbol);
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "pacifica: connection pool is full")};
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
            pool_.broadcast(ep::k_ping_frame);
            state_->next_app_ping_ns = now.ns + ep::k_app_ping_interval_ns;
        }
        if (now.ns >= state_->next_report_ns) {
            if (state_->next_report_ns != 0) {
                report();
            }
            state_->next_report_ns = now.ns + k_report_interval_ns;
        }
    }

private:
    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> ordered{keys.begin(), keys.end()};
        std::ranges::stable_partition(
            ordered, [](const std::string& key) { return key.starts_with("bbo"); });
        std::vector<std::string> frames;
        frames.reserve(ordered.size());
        for (const auto& key : ordered) {
            frames.push_back(frame_for(key));
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
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
        }
        RLOG_INFO("pacifica: {} of {} markets priced, {} traded, {} funded; {} sockets, {} keys, "
                  "{} refusals",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<PacificaData>(std::move(state));
}

} // namespace raikiri::venues::pacifica::internal
