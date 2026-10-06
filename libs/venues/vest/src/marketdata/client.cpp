#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/vest/endpoints.hpp>
#include <raikiri/venues/vest/map.hpp>
#include <raikiri/venues/vest/wire.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::vest::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::uint64_t k_retry_ns = 1'000'000'000ULL;

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_channel(payload)) {
        case Channel::Depth:
            on_depth(payload, received);
            break;
        case Channel::Trades:
            on_trade(payload, received);
            break;
        case Channel::Subscribed:
            state_->subscribe_refusals += count_refusals(payload);
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

    void on_depth(std::string_view payload, UnixNanos received) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value()) {
            return;
        }
        const auto book = map_book(*wire, *found->base, received);
        if (!book.has_value()) {
            return;
        }
        if (const auto top = top_of(*book); top.has_value() && found->market->bbo) {
            state_->sink->on_bbo(*top);
            found->market->priced = true;
        }
        if (found->market->depth) {
            state_->sink->on_depth(*book);
        }
    }

    void on_trade(std::string_view payload, UnixNanos received) {
        const auto wire = parse_trade(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->symbol);
        if (!found.has_value()) {
            return;
        }
        if (const auto trade = map_trade(*wire, *found->base, received); trade.has_value()) {
            state_->sink->on_trade(*trade);
            found->market->traded = true;
        }
    }

    VenueState* state_;
};

class VestData final : public exec::DataClient {
public:
    explicit VestData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_ws_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_rest_host}, .port = "443", .pool_size = 2}},
          gateway_{state_->io,
                   net::RestConfig{.host = std::string{ep::k_gateway_host}, .port = "443", .pool_size = 2}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
        gateway_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        rest_.start();
        gateway_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        pool_.stop();
        rest_.stop();
        gateway_.stop();
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
        std::string_view stream;
        switch (kind) {
        case exec::SubKind::Bbo:
        case exec::SubKind::Depth: {
            const bool asked = market->bbo || market->depth;
            (kind == exec::SubKind::Bbo ? market->bbo : market->depth) = true;
            if (asked) {
                return {};
            }
            stream = "@depth";
            break;
        }
        case exec::SubKind::Trades:
            stream = "@trades";
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = true;
            return {};
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        const std::array<std::string, 1> one{std::string{symbol} + std::string{stream}};
        if (!pool_.subscribe(one)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "vest: connection pool is full")};
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
        gateway_.keepalive(now);
        if (pool_.connected() && now.ns >= next_app_ping_ns_) {
            pool_.broadcast(ep::k_ping_frame);
            next_app_ping_ns_ = now.ns + ep::k_app_ping_interval_ns;
        }
        if (now.ns >= next_tickers_ns_ && !tickers_in_flight_) {
            tickers_in_flight_ = ask(rest_, ep::k_tickers, &VestData::on_tickers, tickers_in_flight_);
            next_tickers_ns_ = now.ns + (tickers_in_flight_ ? ep::k_ticker_interval_ns : k_retry_ns);
        }
        if (now.ns >= next_stats_ns_ && !volume_in_flight_ && !interest_in_flight_) {
            volume_in_flight_ = ask(gateway_, ep::k_volume, &VestData::on_volumes, volume_in_flight_);
            interest_in_flight_ = ask(gateway_, ep::k_open_interest, &VestData::on_interest, interest_in_flight_);
            next_stats_ns_ = now.ns + (volume_in_flight_ && interest_in_flight_ ? ep::k_stats_interval_ns : k_retry_ns);
        }
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    using OnBody = void (VestData::*)(std::string_view);

    bool ask(net::RestClient& client, std::string_view target, OnBody on_body, bool& in_flight) {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target, .body = {}};
        return client.send(request, [this, on_body, &in_flight](net::RestResponse&& response) {
            in_flight = false;
            if (response.status == 200) {
                (this->*on_body)(response.body);
            }
        });
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        if (keys.empty()) {
            return {};
        }
        std::string frame = R"({"method":"SUBSCRIBE","params":[)";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            frame += i == 0 ? "\"" : ",\"";
            frame += keys[i];
            frame += '"';
        }
        frame += R"(],"id":)" + std::to_string(++next_request_id_) + "}";
        return {std::move(frame)};
    }

    [[nodiscard]] std::pair<const InstrumentBase*, Market*> find(std::string_view symbol) const {
        const auto* instrument = state_->instruments.find(symbol);
        if (instrument == nullptr) {
            return {nullptr, nullptr};
        }
        const auto& base = base_of(*instrument);
        return {&base, state_->market_of(base.id)};
    }

    void on_tickers(std::string_view body) {
        static thread_local const auto storage = std::make_unique<TickerBuffer>();
        const auto rows = parse_tickers(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto now = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto [base, market] = find(row.symbol);
            if (market == nullptr) {
                continue;
            }
            if (!market->funding) {
                continue;
            }
            if (const auto funding = map_funding(row, *base, now); funding.has_value()) {
                state_->sink->on_funding(*funding);
                market->funded = true;
            }
        }
    }

    void on_volumes(std::string_view body) {
        static thread_local const auto storage = std::make_unique<StatBuffer>();
        const auto rows = parse_volumes(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            if (auto* market = find(row.symbol).second; market != nullptr) {
                market->stats.volume_base = parse_amount_e9(row.first).value_or(FixedE9{});
                market->stats.volume_quote = parse_turnover_e6(row.second).value_or(FixedE6{});
            }
        }
        publish_stats();
    }

    void on_interest(std::string_view body) {
        static thread_local const auto storage = std::make_unique<StatBuffer>();
        const auto rows = parse_open_interest(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            if (auto* market = find(row.symbol).second; market != nullptr) {
                const auto longs = parse_amount_e9(row.first);
                const auto shorts = parse_amount_e9(row.second);
                market->stats.open_interest = longs.has_value() && shorts.has_value()
                                                  ? FixedE9::from_raw(longs->raw + shorts->raw)
                                                  : FixedE9{};
            }
        }
        publish_stats();
    }

    void publish_stats() {
        const auto now = state_->clock->wall();
        for (std::size_t i = 0; i < state_->markets.size(); ++i) {
            const auto& market = state_->markets[i];
            if (!market.funding) {
                continue;
            }
            const auto* instrument = state_->instruments.find(
                InstrumentId::make(VenueId::Vest, MarketType::Perp, SymbolId{static_cast<std::uint32_t>(i)}));
            if (instrument != nullptr) {
                state_->sink->on_ticker_stats(map_ticker(market.stats, base_of(*instrument), now));
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
        RLOG_INFO("vest: {} of {} markets priced, {} traded, {} funded; {} sockets, {} keys, {} refusals",
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
    net::RestClient rest_;
    net::RestClient gateway_;
    bool started_{false};
    bool tickers_in_flight_{false};
    bool volume_in_flight_{false};
    bool interest_in_flight_{false};
    std::uint64_t next_request_id_{0};
    std::uint64_t next_app_ping_ns_{0};
    std::uint64_t next_tickers_ns_{0};
    std::uint64_t next_stats_ns_{0};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<VestData>(std::move(state));
}

} // namespace raikiri::venues::vest::internal
