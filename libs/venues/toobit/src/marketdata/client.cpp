#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/toobit/endpoints.hpp>
#include <raikiri/venues/toobit/map.hpp>
#include <raikiri/venues/toobit/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::toobit::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::size_t k_symbols_per_frame = 50;
constexpr char k_key_sep = '\x1f';

constexpr std::string_view k_bbo = "bookTicker";
constexpr std::string_view k_depth = "diffDepth";
constexpr std::string_view k_trade = "trade";
constexpr std::string_view k_mark = "markPrice";

[[nodiscard]] std::string_view topic_of(exec::SubKind kind, MarketType market) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return k_bbo;
    case exec::SubKind::Depth:
        return k_depth;
    case exec::SubKind::Trades:
        return k_trade;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return market == MarketType::Perp ? k_mark : std::string_view{};
    case exec::SubKind::Kline:
        break;
    }
    return {};
}

[[nodiscard]] std::string key_of(std::string_view topic, std::string_view symbol) {
    std::string key{topic};
    key.push_back(k_key_sep);
    key.append(symbol);
    return key;
}

[[nodiscard]] std::string frame_for(std::string_view topic, std::string_view symbols, std::string_view event) {
    std::string frame = R"({"symbol":")";
    frame.append(symbols);
    frame.append(R"(","topic":")");
    frame.append(topic);
    frame.append(R"(","event":")");
    frame.append(event);
    frame.append(R"(","params":{"binary":false}})");
    return frame;
}

[[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
    std::vector<std::pair<std::string_view, std::string>> pending;
    std::vector<std::string> frames;
    std::vector<std::size_t> counts;
    for (const std::string_view key : keys) {
        const auto cut = key.find(k_key_sep);
        if (cut == std::string_view::npos) {
            continue;
        }
        const auto topic = key.substr(0, cut);
        const auto symbol = key.substr(cut + 1);
        std::size_t slot = 0;
        while (slot < pending.size() && pending[slot].first != topic) {
            ++slot;
        }
        if (slot == pending.size()) {
            pending.emplace_back(topic, std::string{});
            counts.push_back(0);
        }
        auto& symbols = pending[slot].second;
        if (!symbols.empty()) {
            symbols.push_back(',');
        }
        symbols.append(symbol);
        if (++counts[slot] == k_symbols_per_frame) {
            frames.push_back(frame_for(topic, symbols, "sub"));
            symbols.clear();
            counts[slot] = 0;
        }
    }
    for (const auto& [topic, symbols] : pending) {
        if (!symbols.empty()) {
            frames.push_back(frame_for(topic, symbols, "sub"));
        }
    }
    return frames;
}

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void set_pool(net::WsPool* pool) noexcept { pool_ = pool; }

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto head = parse_head(payload);
        if (!head.has_value()) {
            return;
        }
        switch (head->channel) {
        case Channel::Bbo:
            on_bbo(payload, received);
            break;
        case Channel::Depth:
            on_depth(payload, received);
            break;
        case Channel::Trade:
            on_trades(payload, received);
            break;
        case Channel::Mark:
            on_mark(payload, received);
            break;
        case Channel::Error:
            ++state_->subscribe_refusals;
            break;
        case Channel::Pong:
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

    void on_bbo(std::string_view payload, UnixNanos received) {
        const auto wire = parse_bbo(payload);
        if (!wire.has_value() || !wire->has_value()) {
            return;
        }
        const auto found = lookup((*wire)->symbol);
        if (!found.has_value()) {
            return;
        }
        if (const auto bbo = map_bbo(**wire, *found->base, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            found->market->priced = true;
        }
    }

    void on_depth(std::string_view payload, UnixNanos received) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_depth(payload, storage);
        if (!wire.has_value() || !wire->has_value()) {
            return;
        }
        const auto& depth = **wire;
        const auto found = lookup(depth.symbol);
        if (!found.has_value()) {
            return;
        }
        auto& book = found->market->book;
        switch (book.apply(depth, *found->base)) {
        case DepthBook::Step::Waiting:
            return;
        case DepthBook::Step::Broken:
            resubscribe_depth(depth.symbol);
            return;
        case DepthBook::Step::Applied:
            break;
        }
        found->market->booked = true;
        if (!state_->sink->wants_depth(found->base->id, received)) {
            return;
        }
        if (const auto snapshot = book.snapshot(found->base->id, received); snapshot.has_value()) {
            state_->sink->on_depth(*snapshot);
        }
    }

    void resubscribe_depth(std::string_view symbol) {
        ++state_->book_breaks;
        if (pool_ == nullptr) {
            return;
        }
        std::vector<std::string> frames;
        frames.push_back(frame_for(k_depth, symbol, "cancel"));
        frames.push_back(frame_for(k_depth, symbol, "sub"));
        (void)pool_->requeue(key_of(k_depth, symbol), std::move(frames));
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local PrintBuffer storage;
        const auto wire = parse_trades(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value()) {
            return;
        }
        auto* market = found->market;
        for (const auto& row : wire->rows) {
            if (row.id <= market->last_trade_id) {
                continue;
            }
            market->last_trade_id = row.id;
            if (wire->first) {
                continue;
            }
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                market->traded = true;
            }
        }
    }

    void on_mark(std::string_view payload, UnixNanos received) {
        const auto wire = parse_mark(payload);
        if (!wire.has_value() || !wire->has_value()) {
            return;
        }
        const auto found = lookup((*wire)->symbol);
        if (!found.has_value()) {
            return;
        }
        const auto mark = map_mark(**wire, *found->base);
        if (!mark.has_value()) {
            return;
        }
        auto* market = found->market;
        market->mark = *mark;
        if (!market->wants_funding || !market->funding.has_value()) {
            return;
        }
        auto update = *market->funding;
        update.mark_price = *mark;
        update.ts_event = ms_to_unix((*wire)->ts_ms, received);
        update.ts_init = received;
        state_->sink->on_funding(update);
    }

    VenueState* state_;
    net::WsPool* pool_{nullptr};
};

class ToobitData final : public exec::DataClient {
public:
    explicit ToobitData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_ws_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_rest_host}, .port = "443"}} {
        handler_.set_pool(&pool_);
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        rest_.stop();
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
        const auto topic = topic_of(kind, instrument.market());
        if (topic.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        market->wants_ticker = true;
        if (kind == exec::SubKind::Funding) {
            market->wants_funding = true;
            state_->next_funding_ns = 0;
        }
        const std::array<std::string, 1> one{key_of(topic, symbol)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "toobit: connection pool is full")};
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
        rest_.keepalive(now);
        if (pool_.connected() && now.ns >= state_->next_ping_ns) {
            pool_.broadcast(ping_frame(now));
            state_->next_ping_ns = now.ns + ep::k_app_ping_interval_ns;
        }
        if (now.ns >= state_->next_funding_ns) {
            state_->next_funding_ns = now.ns + ep::k_funding_poll_ns;
            poll_funding();
        }
        if (now.ns >= state_->next_ticker_ns) {
            state_->next_ticker_ns = now.ns + ep::k_ticker_poll_ns;
            poll_tickers(ep::k_contract_ticker);
            poll_tickers(ep::k_spot_ticker);
        }
        if (state_->tick_hook) {
            state_->tick_hook(now);
        }
        if (now.ns >= state_->next_report_ns) {
            if (state_->next_report_ns != 0) {
                report();
            }
            state_->next_report_ns = now.ns + k_report_interval_ns;
        }
    }

private:
    [[nodiscard]] static std::string ping_frame(UnixNanos now) {
        return R"({"ping":)" + std::to_string(now.ns / 1'000'000ULL) + "}";
    }

    [[nodiscard]] bool any(bool Market::*flag) const noexcept {
        for (const auto& market : state_->markets) {
            if (market.*flag) {
                return true;
            }
        }
        return false;
    }

    void poll_funding() {
        if (!any(&Market::wants_funding)) {
            return;
        }
        net::RestRequest request{};
        request.method = net::HttpMethod::Get;
        request.target = ep::k_funding_rate;
        (void)rest_.send(request, [state = state_](net::RestResponse&& response) {
            if (response.transport_failed() || response.status != 200) {
                return;
            }
            static thread_local const auto storage = std::make_unique<FundingBuffer>();
            const auto rows = parse_funding_rates(response.body, *storage);
            if (!rows.has_value()) {
                return;
            }
            const auto now = state->clock->wall();
            for (const auto& row : *rows) {
                const auto* instrument = state->instruments.find(row.symbol);
                if (instrument == nullptr) {
                    continue;
                }
                const auto& base = base_of(*instrument);
                auto* market = state->market_of(base.id);
                if (market == nullptr || !market->wants_funding) {
                    continue;
                }
                if (const auto update = map_funding(row, base, market->mark, now); update.has_value()) {
                    market->funding = *update;
                    market->funded = true;
                    state->sink->on_funding(*update);
                }
            }
        });
    }

    void poll_tickers(std::string_view target) {
        if (!any(&Market::wants_ticker)) {
            return;
        }
        net::RestRequest request{};
        request.method = net::HttpMethod::Get;
        request.target = target;
        (void)rest_.send(request, [state = state_](net::RestResponse&& response) {
            if (response.transport_failed() || response.status != 200) {
                return;
            }
            static thread_local const auto storage = std::make_unique<TickerBuffer>();
            const auto rows = parse_tickers(response.body, *storage);
            if (!rows.has_value()) {
                return;
            }
            const auto now = state->clock->wall();
            for (const auto& row : *rows) {
                const auto* instrument = state->instruments.find(row.symbol);
                if (instrument == nullptr) {
                    continue;
                }
                const auto& base = base_of(*instrument);
                const auto* market = state->market_of(base.id);
                if (market == nullptr || !market->wants_ticker) {
                    continue;
                }
                if (const auto stats = map_ticker(row, base, now); stats.has_value()) {
                    state->sink->on_ticker_stats(*stats);
                }
            }
        });
    }

    void report() const {
        if (::raikiri::log::logger() == nullptr) {
            return;
        }
        std::size_t priced = 0;
        std::size_t traded = 0;
        std::size_t funded = 0;
        std::size_t booked = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
            booked += market.booked ? 1U : 0U;
        }
        RLOG_INFO("toobit: {} of {} markets priced, {} booked, {} traded, {} funded; {} sockets, "
                  "{} keys, {} refusals, {} book resyncs",
                  priced,
                  state_->markets.size(),
                  booked,
                  traded,
                  funded,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  state_->book_breaks);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<ToobitData>(std::move(state));
}

} // namespace raikiri::venues::toobit::internal
