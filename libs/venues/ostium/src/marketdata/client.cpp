#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/ostium/endpoints.hpp>
#include <raikiri/venues/ostium/map.hpp>
#include <raikiri/venues/ostium/wire.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::ostium::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::uint64_t k_ns_per_s = 1'000'000'000ULL;

struct Recorded {
    std::string id;
    std::uint64_t executed_s{0};
    std::uint32_t symbol{0};
    QtyLots qty{};
    FixedE6 quote{};
};

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_frame(payload)) {
        case Frame::Snapshot:
        case Frame::Tick:
            on_quotes(payload, received);
            break;
        case Frame::Error:
            ++state_->subscribe_refusals;
            break;
        case Frame::Gap:
        case Frame::Ack:
        case Frame::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    void on_quotes(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<QuoteBuffer>();
        const auto rows = parse_quotes(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto* instrument = state_->instruments.find(row.pair);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            auto* market = state_->market_of(base.id);
            if (market == nullptr) {
                continue;
            }
            if (const auto mid = mid_ticks(row); mid.has_value()) {
                market->mid = *mid;
            }
            if (!market->bbo) {
                continue;
            }
            if (const auto bbo = map_bbo(row, base, received); bbo.has_value()) {
                state_->sink->on_bbo(*bbo);
                market->priced = true;
            }
        }
    }

    VenueState* state_;
};

class OstiumData final : public exec::DataClient {
public:
    explicit OstiumData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_host}, .port = "443", .pool_size = 1}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        rest_.start();
        started_ = true;
        started_s_ = state_->clock->wall().ns / k_ns_per_s;
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
        switch (kind) {
        case exec::SubKind::Bbo: {
            market->bbo = true;
            const std::array<std::string, 1> one{std::string{symbol}};
            if (!pool_.subscribe(one)) {
                return std::unexpected{
                    exec::VenueError::make(exec::ErrorClass::Terminal, 0, "ostium: connection pool is full")};
            }
            return {};
        }
        case exec::SubKind::Trades:
            market->trades = true;
            return {};
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = true;
            return {};
        case exec::SubKind::Depth:
        case exec::SubKind::Kline:
            break;
        }
        return DataClient::subscribe(kind, instrument);
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
        if (now.ns >= next_rollover_ns_ && !rollover_in_flight_) {
            ask_rollover(now);
        }
        if (now.ns >= next_fills_ns_ && !fills_in_flight_) {
            ask_fills(now);
        }
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        if (keys.empty()) {
            return {};
        }
        std::string frame = R"({"type":"subscribe","pairs":[)";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            frame += i == 0 ? "\"" : ",\"";
            frame += keys[i];
            frame += '"';
        }
        frame += "]}";
        return {std::move(frame)};
    }

    void ask_rollover(UnixNanos now) {
        const net::RestRequest request{
            .method = net::HttpMethod::Post, .target = ep::k_subgraph, .body = ep::k_rollover_query};
        rollover_in_flight_ = rest_.send(request, [this](net::RestResponse&& response) {
            rollover_in_flight_ = false;
            on_rollover(response);
        });
        next_rollover_ns_ = now.ns + (rollover_in_flight_ ? ep::k_rollover_interval_ns : k_ns_per_s);
    }

    void ask_fills(UnixNanos now) {
        const auto now_s = now.ns / k_ns_per_s;
        const auto since = last_executed_s_ != 0 ? last_executed_s_ - std::min(last_executed_s_, ep::k_fills_overlap_s)
                                                 : now_s - ep::k_window_s;
        fills_body_ = std::string{ep::k_fills_head} + std::to_string(since) + std::string{ep::k_fills_tail};
        const net::RestRequest request{.method = net::HttpMethod::Post, .target = ep::k_subgraph, .body = fills_body_};
        fills_in_flight_ = rest_.send(request, [this](net::RestResponse&& response) {
            fills_in_flight_ = false;
            on_fills(response);
        });
        next_fills_ns_ = now.ns + (fills_in_flight_ ? ep::k_fills_interval_ns : k_ns_per_s);
    }

    [[nodiscard]] const InstrumentBase* base_by_pair(std::string_view pair_id) const {
        const auto found = state_->by_pair_id.find(std::string{pair_id});
        if (found == state_->by_pair_id.end()) {
            return nullptr;
        }
        const auto* instrument = state_->instruments.find(
            InstrumentId::make(VenueId::Ostium, MarketType::Perp, SymbolId{found->second}));
        return instrument != nullptr ? &base_of(*instrument) : nullptr;
    }

    void on_rollover(const net::RestResponse& response) {
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<RolloverBuffer>();
        const auto rows = parse_rollover(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto now = state_->clock->wall();
        expire(now.ns / k_ns_per_s);
        for (const auto& row : *rows) {
            const auto* base = base_by_pair(row.id);
            auto* market = base != nullptr ? state_->market_of(base->id) : nullptr;
            if (market == nullptr || !market->funding) {
                continue;
            }
            if (auto funding = map_funding(row, *base, market->mid, now); funding.has_value()) {
                state_->sink->on_funding(*funding);
                market->funded = true;
            }
            std::int64_t qty = 0;
            std::int64_t quote = 0;
            for (const auto& fill : window_) {
                if (fill.symbol == base->id.symbol().raw) {
                    qty += fill.qty.raw;
                    quote += fill.quote.raw;
                }
            }
            state_->sink->on_ticker_stats(
                map_ticker(row, *base, FixedE9::from_raw(qty * base->lot_size.raw), FixedE6::from_raw(quote), now));
        }
    }

    void on_fills(const net::RestResponse& response) {
        if (response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<FillBuffer>();
        const auto rows = parse_fills(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto now = state_->clock->wall();
        for (const auto& row : *rows) {
            last_executed_s_ = std::max(last_executed_s_, row.executed_s);
            if (!seen_.emplace(row.id).second) {
                continue;
            }
            const auto* base = base_by_pair(row.pair_id);
            auto* market = base != nullptr ? state_->market_of(base->id) : nullptr;
            if (market == nullptr) {
                continue;
            }
            const auto print = map_fill(row, *base, now);
            if (!print.has_value()) {
                continue;
            }
            window_.push_back({.id = std::string{row.id},
                               .executed_s = row.executed_s,
                               .symbol = base->id.symbol().raw,
                               .qty = print->trade.qty,
                               .quote = print->quote});
            // the first pass is the day behind us: volume, not the tape
            if (market->trades && row.executed_s >= started_s_) {
                state_->sink->on_trade(print->trade);
                market->traded = true;
            }
        }
    }

    void expire(std::uint64_t now_s) {
        const auto horizon = now_s - std::min(now_s, ep::k_window_s);
        std::erase_if(window_, [&](const Recorded& fill) {
            if (fill.executed_s >= horizon) {
                return false;
            }
            seen_.erase(fill.id);
            return true;
        });
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
        RLOG_INFO("ostium: {} of {} pairs priced, {} traded, {} funded; {} fills in the day window, {} refusals",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  window_.size(),
                  state_->subscribe_refusals);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    bool started_{false};
    bool rollover_in_flight_{false};
    bool fills_in_flight_{false};
    std::uint64_t started_s_{0};
    std::uint64_t next_rollover_ns_{0};
    std::uint64_t next_fills_ns_{0};
    std::uint64_t next_report_ns_{0};
    std::uint64_t last_executed_s_{0};
    std::string fills_body_;
    std::vector<Recorded> window_;
    std::unordered_set<std::string> seen_;
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OstiumData>(std::move(state));
}

} // namespace raikiri::venues::ostium::internal
