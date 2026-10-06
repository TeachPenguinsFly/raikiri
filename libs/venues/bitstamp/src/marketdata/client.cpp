#include <raikiri/exec/events.hpp>
#include <raikiri/exec/ticker_poll.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bitstamp/endpoints.hpp>
#include <raikiri/venues/bitstamp/map.hpp>
#include <raikiri/venues/bitstamp/wire.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bitstamp::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;

// "BTC/USD-PERP" names the market btcusd-perp
std::string symbol_of_pair(std::string_view pair) {
    std::string out;
    out.reserve(pair.size());
    for (const char c : pair) {
        if (c != '/') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        const auto head = parse_head(payload);
        switch (head.event) {
        case Event::Book:
            on_book(payload, symbol_of(head.channel), received);
            break;
        case Event::Trade:
            on_trade(payload, symbol_of(head.channel), received);
            break;
        case Event::Funding:
            on_funding(payload, symbol_of(head.channel), received);
            break;
        case Event::Refused:
            ++state_->subscribe_refusals;
            break;
        case Event::Reconnect:
            ++state_->reconnect_requests;
            break;
        case Event::Subscribed:
        case Event::Unknown:
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
        return market == nullptr ? std::nullopt : std::optional{Found{&base, market}};
    }

    // each frame is the whole top hundred, so one older than the last is dropped, not merged
    void on_book(std::string_view payload, std::string_view symbol, UnixNanos received) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        const auto found = lookup(symbol);
        // a side with no orders leaves nothing to quote, which is not a fault
        if (!wire.has_value() || !found.has_value() || wire->bids.empty() || wire->asks.empty()) {
            return;
        }
        if (wire->ts_us < found->market->last_book_us) {
            ++state_->stale_books;
            return;
        }
        const auto book = map_book(*wire, *found->base, received);
        if (!book.has_value()) {
            ++state_->refused_books;
            return;
        }
        found->market->last_book_us = wire->ts_us;
        state_->sink->on_bbo(Bbo{.instrument = book->instrument,
                                 .bid = book->bids[0].price,
                                 .ask = book->asks[0].price,
                                 .bid_qty = book->bids[0].qty,
                                 .ask_qty = book->asks[0].qty,
                                 .ts_event = book->ts_event,
                                 .ts_init = received});
        found->market->priced = true;
        if (found->market->depth && state_->sink->wants_depth(book->instrument, received)) {
            state_->sink->on_depth(*book);
        }
    }

    void on_trade(std::string_view payload, std::string_view symbol, UnixNanos received) {
        const auto wire = parse_trade(payload);
        const auto found = lookup(symbol);
        if (!wire.has_value() || !found.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, *found->base, received); trade.has_value()) {
            state_->sink->on_trade(*trade);
            found->market->traded = true;
        }
    }

    void on_funding(std::string_view payload, std::string_view symbol, UnixNanos received) {
        const auto wire = parse_funding(payload);
        const auto found = lookup(symbol);
        if (!wire.has_value() || !found.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, *found->base, received); funding.has_value()) {
            state_->sink->on_funding(*funding);
            found->market->funded = true;
        }
    }

    VenueState* state_;
};

class BitstampData final : public exec::DataClient {
public:
    explicit BitstampData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_ws_host}, .port = "443", .target = "/"},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_host}, .port = "443", .timeout_ms = 20'000}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        rest_.start();
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
        std::string channel;
        switch (kind) {
        case exec::SubKind::Bbo:
        case exec::SubKind::Depth:
            if (kind == exec::SubKind::Bbo) {
                volumes_.want(instrument);
            }
            market->depth = market->depth || kind == exec::SubKind::Depth;
            channel = std::format("order_book_{}", symbol);
            break;
        case exec::SubKind::Trades:
            channel = std::format("live_trades_{}", symbol);
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            if (instrument.market() != MarketType::Perp) {
                return DataClient::subscribe(kind, instrument);
            }
            channel = std::format("funding_rate_{}", symbol);
            break;
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (!subscribed_.insert(channel).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(channel)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitstamp: connection pool is full")};
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
        volumes_.poll(now, [this] {
            const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_tickers};
            return rest_.send(request, [this](const net::RestResponse& response) { on_tickers(response); });
        });
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    // one channel a frame; books first so quotes arrive while the rest is still paced out
    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> ordered{keys.begin(), keys.end()};
        std::ranges::stable_partition(ordered, [](std::string_view key) { return key.starts_with("order_book_"); });
        std::vector<std::string> frames;
        frames.reserve(ordered.size());
        for (const auto key : ordered) {
            frames.push_back(std::format(R"({{"event":"bts:subscribe","data":{{"channel":"{}"}}}})", key));
        }
        return frames;
    }

    void on_tickers(const net::RestResponse& response) noexcept {
        volumes_.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(symbol_of_pair(row.pair));
            if (instrument == nullptr || !volumes_.wanted(base_of(*instrument).id)) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (const auto stats = map_ticker_stats(row, base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
                if (auto* market = state_->market_of(base.id); market != nullptr) {
                    market->volumed = true;
                }
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
        std::size_t volumed = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
            volumed += market.volumed ? 1U : 0U;
        }
        RLOG_INFO("bitstamp {}: {} of {} markets priced two-sided, {} traded, {} funded, {} with volume; "
                  "{} sockets, {} keys, {} refusals, {} reconnect requests, {} stale and {} refused books",
                  to_string(state_->market),
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  volumed,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  state_->reconnect_requests,
                  state_->stale_books,
                  state_->refused_books);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    exec::TickerPoll volumes_;
    std::unordered_set<std::string> subscribed_;
    bool started_{false};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitstampData>(std::move(state));
}

} // namespace raikiri::venues::bitstamp::internal
