#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/derive/endpoints.hpp>
#include <raikiri/venues/derive/map.hpp>
#include <raikiri/venues/derive/wire.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::derive::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::size_t k_default_channels_per_frame = 500;

[[nodiscard]] std::string channel_for(exec::SubKind kind, std::string_view symbol) {
    switch (kind) {
    case exec::SubKind::Bbo:
        return std::format("orderbook.{}.1.1", symbol);
    case exec::SubKind::Depth:
        return std::format("orderbook.{}.1.10", symbol);
    case exec::SubKind::Trades:
        return std::format("trades.{}", symbol);
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return std::format("ticker_slim.{}.100", symbol);
    case exec::SubKind::Kline:
        break;
    }
    return {};
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
        switch (head.channel) {
        case Channel::Book:
            on_book(payload, received, head.name.ends_with(".1.1"));
            break;
        case Channel::Ticker:
            on_ticker(payload, instrument_of(head.name, 1), received);
            break;
        case Channel::Trades:
            on_trades(payload, received);
            break;
        case Channel::Response:
            on_response(payload);
            break;
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

    void on_book(std::string_view payload, UnixNanos received, bool top) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->instrument);
        if (!found.has_value()) {
            return;
        }
        if (top) {
            if (const auto bbo = map_bbo(*wire, *found->base, received); bbo.has_value()) {
                state_->sink->on_bbo(*bbo);
                found->market->priced = true;
            }
            return;
        }
        if (const auto book = map_book(*wire, *found->base, received); book.has_value()) {
            state_->sink->on_depth(*book);
        }
    }

    void on_ticker(std::string_view payload, std::string_view symbol, UnixNanos received) {
        const auto wire = parse_ticker(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(symbol);
        if (!found.has_value()) {
            return;
        }
        if (const auto funding = map_funding(*wire, *found->base, received); funding.has_value()) {
            state_->sink->on_funding(*funding);
            found->market->funded = true;
        }
        if (const auto greeks = map_greeks(*wire, *found->base, received); greeks.has_value()) {
            state_->sink->on_option_greeks(*greeks);
            found->market->greeked = true;
        }
        if (const auto stats = map_ticker_stats(*wire, *found->base, state_->market, received); stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
        }
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local TradeBuffer storage;
        const auto wire = parse_trades(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        for (const auto& print : *wire) {
            const auto found = lookup(print.instrument);
            if (!found.has_value()) {
                continue;
            }
            if (const auto trade = map_trade(print, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    // a refused channel is named in the status map, or the whole call fails with an error
    void on_response(std::string_view payload) {
        if (const auto ack = parse_subscribe_ack(payload); ack.has_value()) {
            state_->subscribe_refusals += ack->refused;
            return;
        }
        if (parse_rpc_error(payload).has_value()) {
            ++state_->subscribe_refusals;
        }
    }

    VenueState* state_;
};

class DeriveData final : public exec::DataClient {
public:
    explicit DeriveData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {}

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
        if (symbol.empty() || state_->market_of(instrument) == nullptr) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        auto channel = channel_for(kind, symbol);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        const std::array<std::string, 1> one{std::move(channel)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "derive: connection pool is full")};
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
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    // called on every open, so the top-of-book channels go first
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        const auto limit = net::process_ws_limits().subscriptions_per_frame;
        const auto per_frame = limit == 0 ? k_default_channels_per_frame : limit;
        std::vector<std::string_view> ordered{keys.begin(), keys.end()};
        std::ranges::stable_partition(ordered, [](std::string_view key) { return key.ends_with(".1.1"); });
        std::vector<std::string> frames;
        for (std::size_t at = 0; at < ordered.size(); at += per_frame) {
            std::string frame = R"({"method":"subscribe","params":{"channels":[)";
            const auto end = std::min(ordered.size(), at + per_frame);
            for (std::size_t i = at; i < end; ++i) {
                if (i != at) {
                    frame += ',';
                }
                frame += '"';
                frame += ordered[i];
                frame += '"';
            }
            frame += std::format(R"(]}},"id":{}}})", ++next_id_);
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    void report() const {
        if (::raikiri::log::logger() == nullptr) {
            return;
        }
        std::size_t priced = 0;
        std::size_t greeked = 0;
        std::size_t traded = 0;
        std::size_t funded = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            greeked += market.greeked ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
        }
        RLOG_INFO("derive {}: {} of {} markets priced two-sided, {} with greeks, {} traded, {} funded; "
                  "{} sockets, {} keys, {} refusals",
                  to_string(state_->market),
                  priced,
                  state_->markets.size(),
                  greeked,
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
    std::uint64_t next_id_{0};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<DeriveData>(std::move(state));
}

} // namespace raikiri::venues::derive::internal
