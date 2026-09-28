#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_limits.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/map.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::bitunix::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::size_t k_default_args_per_frame = 50;
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string_view channel_of(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "depth_book1";
    case exec::SubKind::Depth:
        return "depth_book15";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        break;
    }
    return {};
}

[[nodiscard]] std::string ping_frame(UnixNanos now) {
    return R"({"op":"ping","ping":)" + std::to_string(now.ns / 1'000'000'000ULL) + "}";
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
            on_book(payload, received, head.ch == "depth_book1");
            break;
        case Channel::Trade:
            on_trades(payload, received);
            break;
        case Channel::Refused:
            ++state_->subscribe_refusals;
            break;
        case Channel::Pong:
        case Channel::Connect:
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

    // the top and the ladder are separate subscriptions on one cadence; each publishes its own
    void on_book(std::string_view payload, UnixNanos received, bool top) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
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
        for (const auto& print : wire->prints) {
            if (const auto trade = map_trade(print, wire->ts_ms, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    VenueState* state_;
};

class BitunixData final : public exec::DataClient {
public:
    explicit BitunixData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{
              state_->io,
              net::WsConfig{.host = std::string{ep::k_perp_host}, .port = "443", .target = std::string{ep::k_ws_path}},
              handler_,
              net::WsPoolConfig{},
              [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{state_->rest_host()}}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        if (state_->market == MarketType::Perp) {
            pool_.start();
        }
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return state_->market == MarketType::Spot ? started_ : pool_.connected();
    }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return state_->subscribe_refusals; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto symbol = state_->instruments.exchange_symbol(instrument);
        auto* market = state_->market_of(instrument);
        if (symbol.empty() || market == nullptr) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        if (kind == exec::SubKind::Funding || kind == exec::SubKind::MarkPrice) {
            if (state_->market != MarketType::Perp) {
                return DataClient::subscribe(kind, instrument);
            }
            market->funding = true;
            return {};
        }
        const auto channel = channel_of(kind);
        if (channel.empty()) {
            return DataClient::subscribe(kind, instrument);
        }
        if (state_->market == MarketType::Spot) {
            if (kind == exec::SubKind::Trades) {
                return DataClient::subscribe(kind, instrument);
            }
            if (std::ranges::find(spot_books_, instrument) == spot_books_.end()) {
                spot_books_.push_back(instrument);
            }
            return {};
        }
        std::string key{channel};
        key.push_back(k_key_sep);
        key.append(symbol);
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "bitunix: connection pool is full")};
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
        if (pool_.connected() && now.ns >= next_app_ping_ns_) {
            pool_.broadcast(ping_frame(now));
            next_app_ping_ns_ = now.ns + ep::k_app_ping_interval_ns;
        }
        poll_funding(now);
        poll_spot(now);
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        const auto limit = net::process_ws_limits().subscriptions_per_frame;
        const auto per_frame = limit == 0 ? k_default_args_per_frame : limit;
        std::vector<std::string> ordered{keys.begin(), keys.end()};
        std::ranges::stable_partition(ordered, [](const std::string& key) { return key.starts_with("depth_book1"); });
        std::vector<std::string> frames;
        for (std::size_t at = 0; at < ordered.size(); at += per_frame) {
            std::string frame = R"({"op":"subscribe","args":[)";
            const auto end = std::min(ordered.size(), at + per_frame);
            for (std::size_t i = at; i < end; ++i) {
                const std::string_view key = ordered[i];
                const auto cut = key.find(k_key_sep);
                if (i != at) {
                    frame += ',';
                }
                frame += R"({"symbol":")";
                frame += key.substr(cut + 1);
                frame += R"(","ch":")";
                frame += key.substr(0, cut);
                frame += R"("})";
            }
            frame += "]}";
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    [[nodiscard]] bool wants_funding() const noexcept {
        return std::ranges::any_of(state_->markets, [](const Market& m) { return m.funding; });
    }

    void poll_funding(UnixNanos now) noexcept {
        if (state_->market != MarketType::Perp || in_flight_ != 0 || now.ns < next_funding_poll_ns_ ||
            !wants_funding()) {
            return;
        }
        const net::RestRequest funding{.method = net::HttpMethod::Get, .target = ep::k_funding_batch};
        const net::RestRequest tickers{.method = net::HttpMethod::Get, .target = ep::k_tickers};
        try {
            if (rest_.send(funding, [this](net::RestResponse&& r) { on_funding(r); })) {
                ++in_flight_;
            }
            if (rest_.send(tickers, [this](net::RestResponse&& r) { on_tickers(r); })) {
                ++in_flight_;
            }
        } catch (...) {
            return;
        }
        if (in_flight_ != 0) {
            next_funding_poll_ns_ = now.ns + ep::k_funding_poll_ns;
        }
    }

    void poll_spot(UnixNanos now) noexcept {
        if (state_->market != MarketType::Spot || spot_books_.empty() || in_flight_ != 0 ||
            now.ns < next_spot_poll_ns_) {
            return;
        }
        next_spot_poll_ns_ = now.ns + ep::k_spot_poll_ns;
        spot_cursor_ %= spot_books_.size();
        const auto id = spot_books_[spot_cursor_];
        ++spot_cursor_;
        const auto* instrument = state_->instruments.find(id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        spot_target_.assign(ep::k_spot_depth);
        spot_target_ += "?symbol=";
        spot_target_ += base.exchange_symbol.view();
        spot_target_ += "&precision=";
        spot_target_ += precision_text(base.tick_size);
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = spot_target_};
        try {
            if (rest_.send(request, [this, id](net::RestResponse&& r) { on_spot_depth(r, id); })) {
                ++in_flight_;
            }
        } catch (...) {
            return;
        }
    }

    void on_spot_depth(const net::RestResponse& response, InstrumentId id) noexcept {
        if (!usable(response)) {
            return;
        }
        static thread_local LevelBuffer storage;
        const auto wire = parse_spot_depth(response.body, storage);
        const auto* instrument = state_->instruments.find(id);
        auto* market = state_->market_of(id);
        if (!wire.has_value() || instrument == nullptr || market == nullptr) {
            return;
        }
        const auto received = state_->clock->wall();
        const auto& base = base_of(*instrument);
        if (const auto bbo = map_bbo(*wire, base, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            market->priced = true;
        }
        if (const auto book = map_book(*wire, base, received); book.has_value()) {
            state_->sink->on_depth(*book);
        }
    }

    // the venue answers an overload as http 200 with code 100009 and no data
    [[nodiscard]] bool usable(const net::RestResponse& response) noexcept {
        if (in_flight_ > 0) {
            --in_flight_;
        }
        if (response.transport_failed() || response.status != 200) {
            return false;
        }
        const auto status = parse_status(response.body);
        if (status.has_value() && status->code == k_rate_limited) {
            ++rate_limited_;
            next_funding_poll_ns_ = state_->clock->wall().ns + ep::k_funding_poll_ns;
            next_spot_poll_ns_ = state_->clock->wall().ns + 1'000'000'000ULL;
            return false;
        }
        return status.has_value() && status->code == k_ok;
    }

    void on_funding(const net::RestResponse& response) noexcept {
        if (!usable(response)) {
            return;
        }
        static thread_local const auto storage = std::make_unique<FundingBuffer>();
        const auto rows = parse_funding(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            auto* market = state_->market_of(base.id);
            if (market == nullptr || !market->funding) {
                continue;
            }
            if (const auto update = map_funding(row, base, received); update.has_value()) {
                state_->sink->on_funding(*update);
                market->funded = true;
            }
        }
    }

    void on_tickers(const net::RestResponse& response) noexcept {
        if (!usable(response)) {
            return;
        }
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.symbol);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            const auto* market = state_->market_of(base.id);
            if (market == nullptr || !market->funding) {
                continue;
            }
            if (const auto stats = map_ticker_stats(row, base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
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
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            funded += market.funded ? 1U : 0U;
        }
        RLOG_INFO("bitunix {}: {} of {} markets priced, {} traded, {} funded; {} sockets, {} keys, "
                  "{} refusals, {} rate-limited polls",
                  to_string(state_->market),
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  rate_limited_);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    bool started_{false};
    int in_flight_{0};
    std::uint64_t rate_limited_{0};
    std::uint64_t next_app_ping_ns_{0};
    std::uint64_t next_report_ns_{0};
    std::uint64_t next_funding_poll_ns_{0};
    std::vector<InstrumentId> spot_books_;
    std::size_t spot_cursor_{0};
    std::string spot_target_;
    std::uint64_t next_spot_poll_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BitunixData>(std::move(state));
}

} // namespace raikiri::venues::bitunix::internal
