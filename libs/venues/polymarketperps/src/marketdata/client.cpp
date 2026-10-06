#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/polymarketperps/endpoints.hpp>
#include <raikiri/venues/polymarketperps/map.hpp>
#include <raikiri/venues/polymarketperps/wire.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::polymarketperps::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::uint64_t k_ns_per_ms = 1'000'000ULL;

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        for (auto& market : state_->markets) {
            market.quote = {};
            market.book = {};
        }
        // bbo:: sends changes only, so a quiet market is seeded from rest
        state_->seed_due = true;
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_frame(payload)) {
        case Frame::Bbo:
            on_bbo(payload, received);
            break;
        case Frame::Book:
            on_book(payload, received);
            break;
        case Frame::Trades:
            on_trades(payload, received);
            break;
        case Frame::Ticker:
            on_ticker(payload, received);
            break;
        case Frame::Statistics:
            on_statistics(payload, received);
            break;
        case Frame::Answer:
            state_->subscribe_refusals += count_refusals(payload);
            break;
        case Frame::Unknown:
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

    [[nodiscard]] std::optional<Found> lookup(std::uint64_t iid) noexcept {
        const auto found = state_->by_iid.find(iid);
        if (found == state_->by_iid.end()) {
            return std::nullopt;
        }
        const auto* instrument = state_->instruments.find(
            InstrumentId::make(VenueId::PolymarketPerps, MarketType::Perp, SymbolId{found->second}));
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
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->iid);
        if (!found.has_value() || !found->market->bbo || !found->market->quote.admit(wire->sq)) {
            return;
        }
        if (const auto bbo = map_bbo(*wire, *found->base, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            found->market->priced = true;
        }
    }

    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_book(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->iid);
        if (!found.has_value() || !found->market->book.admit(wire->sq)) {
            return;
        }
        const auto book = map_book(*wire, *found->base, received);
        if (!book.has_value()) {
            return;
        }
        if (const auto top = top_of(*book);
            top.has_value() && found->market->bbo && found->market->quote.admit(wire->sq)) {
            state_->sink->on_bbo(*top);
            found->market->priced = true;
        }
        if (found->market->depth) {
            state_->sink->on_depth(*book);
        }
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<PrintBuffer>();
        const auto rows = parse_trades(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.iid);
            if (!found.has_value()) {
                continue;
            }
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    void on_ticker(std::string_view payload, UnixNanos received) {
        const auto wire = parse_ticker(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->iid);
        if (!found.has_value()) {
            return;
        }
        auto& market = *found->market;
        if (const auto oi = parse_amount_e9(wire->open_interest); oi.has_value()) {
            market.stats.open_interest = *oi;
        }
        const auto at = wire->ts_ms * k_ns_per_ms;
        if (!market.funding || at < market.next_funding_ns) {
            return;
        }
        market.next_funding_ns = at + ep::k_funding_spacing_ns;
        if (const auto funding = map_funding(*wire, *found->base, received); funding.has_value()) {
            state_->sink->on_funding(*funding);
            market.funded = true;
        }
        state_->sink->on_ticker_stats(map_ticker(market.stats, *found->base, received));
    }

    void on_statistics(std::string_view payload, UnixNanos received) {
        const auto wire = parse_statistics(payload);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->iid);
        if (!found.has_value()) {
            return;
        }
        if (const auto volume = parse_turnover_e6(wire->volume); volume.has_value()) {
            found->market->stats.volume_quote = *volume;
        }
        if (found->market->funding) {
            state_->sink->on_ticker_stats(map_ticker(found->market->stats, *found->base, received));
        }
    }

    VenueState* state_;
};

class PolymarketData final : public exec::DataClient {
public:
    explicit PolymarketData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_ws_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_rest_host}, .port = "443", .pool_size = 1}} {
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
        auto* market = state_->market_of(instrument);
        std::uint64_t iid = 0;
        for (const auto& [id, symbol] : state_->by_iid) {
            if (symbol == instrument.symbol().raw) {
                iid = id;
            }
        }
        if (market == nullptr || iid == 0) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::string key;
        switch (kind) {
        case exec::SubKind::Bbo:
            market->bbo = true;
            key = "bbo::" + std::to_string(iid);
            break;
        case exec::SubKind::Depth:
            market->depth = true;
            key = "book::" + std::to_string(iid);
            break;
        case exec::SubKind::Trades:
            market->trades = true;
            key = "trades::" + std::to_string(iid);
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = true;
            if (all_asked_) {
                return {};
            }
            all_asked_ = true;
            return place({std::string{ep::k_tickers}, std::string{ep::k_statistics}});
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        return place({std::move(key)});
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
            pool_.broadcast(ep::k_ping_frame);
            next_app_ping_ns_ = now.ns + ep::k_app_ping_interval_ns;
        }
        if (state_->seed_due && !seed_in_flight_) {
            const net::RestRequest request{.method = net::HttpMethod::Get, .target = ep::k_bbo, .body = {}};
            seed_in_flight_ = rest_.send(request, [this](net::RestResponse&& response) {
                seed_in_flight_ = false;
                if (response.status == 200) {
                    state_->seed_due = false;
                    on_seed(response.body);
                }
            });
        }
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    std::expected<void, exec::VenueError> place(std::vector<std::string> keys) {
        if (!pool_.subscribe(keys)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "polymarketperps: connection pool is full")};
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        if (keys.empty()) {
            return {};
        }
        std::string frame = R"({"id":)" + std::to_string(++next_request_id_) + R"(,"req":"sub","chs":[)";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            frame += i == 0 ? "\"" : ",\"";
            frame += keys[i];
            frame += '"';
        }
        frame += "]}";
        return {std::move(frame)};
    }

    void on_seed(std::string_view body) {
        static thread_local const auto storage = std::make_unique<BboBuffer>();
        const auto rows = parse_bbo_rows(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto now = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto found = state_->by_iid.find(row.iid);
            if (found == state_->by_iid.end()) {
                continue;
            }
            const auto id = InstrumentId::make(VenueId::PolymarketPerps, MarketType::Perp, SymbolId{found->second});
            const auto* instrument = state_->instruments.find(id);
            auto* market = state_->market_of(id);
            if (instrument == nullptr || market == nullptr || !market->bbo || market->quote.last != 0) {
                continue;
            }
            if (const auto bbo = map_bbo(row, base_of(*instrument), now); bbo.has_value()) {
                state_->sink->on_bbo(*bbo);
                market->priced = true;
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
        RLOG_INFO("polymarketperps: {} of {} markets priced, {} traded, {} funded; {} sockets, {} keys, {} refusals",
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
    bool started_{false};
    bool all_asked_{false};
    bool seed_in_flight_{false};
    std::uint64_t next_request_id_{0};
    std::uint64_t next_app_ping_ns_{0};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<PolymarketData>(std::move(state));
}

} // namespace raikiri::venues::polymarketperps::internal
