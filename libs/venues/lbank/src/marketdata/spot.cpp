#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/exec/ticker_poll.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/lbank/endpoints.hpp>
#include <raikiri/venues/lbank/map.hpp>
#include <raikiri/venues/lbank/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::lbank::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr char k_key_sep = '\x1f';

// a key is "<channel>\x1f<pair>"
[[nodiscard]] std::string frame_for(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    const auto channel = key.substr(0, cut);
    std::string frame = R"({"action":"subscribe","subscribe":")" + std::string{channel} + '"';
    if (channel == "depth") {
        frame += R"(,"depth":"10")";
    }
    frame += R"(,"pair":")" + std::string{key.substr(cut + 1)} + R"("})";
    return frame;
}

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_spot_channel(payload)) {
        case SpotChannel::Depth:
            on_depth(payload, received);
            break;
        case SpotChannel::Trade:
            on_trade(payload, received);
            break;
        case SpotChannel::Tick:
            on_tick(payload, received);
            break;
        case SpotChannel::Error:
            ++state_->subscribe_refusals;
            break;
        case SpotChannel::Pong:
        case SpotChannel::Unknown:
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

    [[nodiscard]] std::optional<Found> lookup(std::string_view pair) noexcept {
        const auto* instrument = state_->instruments.find(pair);
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

    void on_depth(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<SpotLevelBuffer>();
        const auto wire = parse_spot_depth(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->pair);
        if (!found.has_value() || wire->ts_ms < found->market->last_bno) {
            return;
        }
        found->market->last_bno = wire->ts_ms;
        const auto book = map_spot_book(*wire, *found->base, received);
        if (!book.has_value()) {
            if (++state_->book_faults <= 3 && ::raikiri::log::logger() != nullptr) {
                RLOG_WARN("lbank: a {} depth frame does not fit its grid", wire->pair);
            }
            return;
        }
        if (const auto top = top_of(*book); top.has_value()) {
            state_->sink->on_bbo(*top);
            found->market->priced = true;
        }
        state_->sink->on_depth(*book);
    }

    void on_trade(std::string_view payload, UnixNanos received) {
        const auto wire = parse_spot_trade(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->pair);
        if (!found.has_value()) {
            return;
        }
        if (const auto trade = map_spot_trade(*wire, *found->base, received); trade.has_value()) {
            state_->sink->on_trade(*trade);
            found->market->traded = true;
        }
    }

    void on_tick(std::string_view payload, UnixNanos received) {
        const auto wire = parse_spot_tick(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->pair);
        if (!found.has_value()) {
            return;
        }
        if (const auto stats = map_spot_stats(*wire, *found->base, received); stats.has_value()) {
            state_->sink->on_ticker_stats(*stats);
            found->market->funded = true;
        }
    }

    VenueState* state_;
};

class SpotData final : public exec::DataClient {
public:
    explicit SpotData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{
                    .host = std::string{ep::k_spot_host}, .port = "443", .target = std::string{ep::k_spot_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::k_spot_host}, .port = "443", .timeout_ms = 20'000}} {
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
        const auto pair = state_->instruments.exchange_symbol(instrument);
        if (pair.empty() || state_->market_of(instrument) == nullptr || instrument.market() != MarketType::Spot) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Bbo) {
            // the tick channel rides funding, which a spot account does not open
            tickers_.want(instrument);
        }
        std::string_view channel;
        switch (kind) {
        case exec::SubKind::Bbo:
        case exec::SubKind::Depth:
            channel = "depth";
            break;
        case exec::SubKind::Trades:
            channel = "trade";
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            channel = "tick";
            break;
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        std::string key = std::string{channel} + k_key_sep + std::string{pair};
        if (!asked_.insert(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "lbank: connection pool is full")};
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
        tickers_.poll(now, [this] {
            const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_spot_tickers, .body = {}};
            return rest_.send(request, [this](const net::RestResponse& response) { on_tickers(response); });
        });
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            pool_.broadcast(ep::k_spot_ping);
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
    void on_tickers(const net::RestResponse& response) noexcept {
        tickers_.answered();
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<SpotTickBuffer>();
        const auto rows = parse_spot_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.pair);
            if (instrument == nullptr || !tickers_.wanted(base_of(*instrument).id)) {
                continue;
            }
            if (const auto stats = map_spot_stats(row, base_of(*instrument), received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
                if (auto* market = state_->market_of(stats->instrument); market != nullptr) {
                    market->funded = true;
                }
            }
        }
    }

    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> frames;
        frames.reserve(keys.size());
        for (const auto& key : keys) {
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
        std::size_t ticked = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            ticked += market.funded ? 1U : 0U;
        }
        RLOG_INFO("lbank: {} of {} spot pairs priced, {} traded, {} with a ticker; {} sockets, {} "
                  "keys, {} refusals, {} bad books",
                  priced,
                  state_->markets.size(),
                  traded,
                  ticked,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  state_->book_faults);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    exec::TickerPoll tickers_;
    std::unordered_set<std::string> asked_;
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    if (state->market == MarketType::Spot) {
        return std::make_unique<SpotData>(std::move(state));
    }
    return make_perp_data_client(std::move(state));
}

} // namespace raikiri::venues::lbank::internal
