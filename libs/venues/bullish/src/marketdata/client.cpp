#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bullish/endpoints.hpp>
#include <raikiri/venues/bullish/map.hpp>
#include <raikiri/venues/bullish/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bullish::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;

[[nodiscard]] std::string frame_for(std::string_view topic, std::string_view symbol) {
    return R"({"jsonrpc":"2.0","type":"command","method":"subscribe","params":{"topic":")" + std::string{topic} +
           R"(","symbol":")" + std::string{symbol} + R"("},"id":"1"})";
}

[[nodiscard]] net::WsPool::BuildFrames builder(std::string_view topic) {
    return [topic](std::span<const std::string> keys) {
        std::vector<std::string> frames;
        frames.reserve(keys.size());
        for (const auto& key : keys) {
            frames.push_back(frame_for(topic, key));
        }
        return frames;
    };
}

[[nodiscard]] net::WsConfig socket(bool testnet, std::string_view target) {
    return net::WsConfig{.host = std::string{ep::host(testnet)}, .port = "443", .target = std::string{target}};
}

class BullishData final : public exec::DataClient, public net::WsHandler {
public:
    explicit BullishData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, books_{state_->io,
                                           socket(state_->testnet, ep::k_ws_orderbook),
                                           *this,
                                           net::WsPoolConfig{},
                                           builder("l2Orderbook")},
          ticks_{state_->io, socket(state_->testnet, ep::k_ws_tick), *this, net::WsPoolConfig{}, builder("tick")},
          trades_{state_->io,
                  socket(state_->testnet, ep::k_ws_trades),
                  *this,
                  net::WsPoolConfig{},
                  builder("anonymousTrades")},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::host(state_->testnet)}, .port = "443", .pool_size = 1}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        for (auto* pool : pools()) {
            pool->start();
        }
        rest_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        for (auto* pool : pools()) {
            pool->stop();
        }
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return books_.connected() || ticks_.connected() || trades_.connected();
    }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        auto* market = state_->market_of(instrument);
        if (symbol.empty() || market == nullptr) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        net::WsPool* pool = nullptr;
        bool* asked = nullptr;
        switch (kind) {
        case exec::SubKind::Bbo:
        case exec::SubKind::Depth:
            pool = &books_;
            asked = &market->booked;
            break;
        case exec::SubKind::Trades:
            pool = &trades_;
            asked = &market->traded_wanted;
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            pool = &ticks_;
            asked = &market->ticked;
            indexed_ = true;
            break;
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (*asked) {
            return {};
        }
        *asked = true;
        const std::array<std::string, 1> one{std::string{symbol}};
        if (!pool->subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bullish: connection pool is full")};
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
        for (auto* pool : pools()) {
            pool->poll(now);
        }
        rest_.keepalive(now);
        if (indexed_ && !index_in_flight_ && now.ns >= next_index_ns_) {
            index_in_flight_ = ask_index();
            next_index_ns_ = now.ns + ep::k_index_interval_ns;
        }
        if (now.ns >= state_->next_keepalive_ns) {
            for (auto* pool : pools()) {
                if (pool->connected()) {
                    pool->broadcast(ep::k_keepalive_frame);
                }
            }
            state_->next_keepalive_ns = now.ns + ep::k_keepalive_interval_ns;
        }
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
        if (head->kind == Kind::Refusal) {
            ++state_->subscribe_refusals;
            return;
        }
        switch (head->data) {
        case DataType::Book:
            on_book(payload, received);
            break;
        case DataType::Tick:
            on_tick(payload, received);
            break;
        case DataType::Trades:
            // the subscribe snapshot replays recent history
            if (head->kind == Kind::Update) {
                on_trades(payload, received);
            }
            break;
        case DataType::Unknown:
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

    [[nodiscard]] std::array<net::WsPool*, 3> pools() noexcept { return {&books_, &ticks_, &trades_}; }

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

    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_book(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value() || !found->market->book.admit(*wire)) {
            return;
        }
        const auto book = map_book(*wire, *found->base, received);
        if (!book.has_value()) {
            return;
        }
        if (const auto top = top_of(*book); top.has_value()) {
            state_->sink->on_bbo(*top);
            found->market->priced = true;
        }
        if (state_->sink->wants_depth(found->base->id, received)) {
            state_->sink->on_depth(*book);
        }
    }

    void on_tick(std::string_view payload, UnixNanos received) {
        const auto wire = parse_tick(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value()) {
            return;
        }
        if (!wire->funding_rate.empty()) {
            if (const auto funding = map_funding(*wire, *found->base, found->market->index, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                found->market->funded = true;
            }
        }
        if (const auto stats = map_ticker_stats(*wire, *found->base, received); stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
            found->market->volume = true;
        }
    }

    bool ask_index() {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_index_prices, .body = {}};
        return rest_.send(request, [this](net::RestResponse&& response) {
            index_in_flight_ = false;
            if (response.status == 200) {
                on_index(response.body);
            }
        });
    }

    void on_index(std::string_view body) {
        static thread_local const auto storage = std::make_unique<IndexBuffer>();
        const auto rows = parse_index_prices(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        std::string symbol;
        for (const auto& row : *rows) {
            symbol.assign(row.asset).append("-USDC-PERP");
            const auto found = lookup(symbol);
            if (!found.has_value()) {
                continue;
            }
            if (const auto index = index_ticks(row, *found->base); index.has_value()) {
                found->market->index = *index;
            }
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

    void report() const {
        if (::raikiri::log::logger() == nullptr) {
            return;
        }
        std::size_t priced = 0;
        std::size_t traded = 0;
        std::size_t funded = 0;
        std::size_t volume = 0;
        std::uint64_t gaps = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
            volume += market.volume ? 1U : 0U;
            gaps += market.book.gaps;
        }
        RLOG_INFO("bullish: {} of {} markets priced, {} traded, {} funded, {} with volume; {} sequence gaps; "
                  "{} sockets, {} refusals",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  volume,
                  gaps,
                  books_.connection_count() + ticks_.connection_count() + trades_.connection_count(),
                  state_->subscribe_refusals);
    }

    std::shared_ptr<VenueState> state_;
    net::WsPool books_;
    net::WsPool ticks_;
    net::WsPool trades_;
    net::RestClient rest_;
    std::uint64_t next_index_ns_{0};
    bool indexed_{false};
    bool index_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BullishData>(std::move(state));
}

} // namespace raikiri::venues::bullish::internal
