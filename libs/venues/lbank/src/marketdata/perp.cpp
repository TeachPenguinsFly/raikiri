#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/lbank/endpoints.hpp>
#include <raikiri/venues/lbank/map.hpp>
#include <raikiri/venues/lbank/wire.hpp>
#include <raikiri/wire/json.hpp>

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
constexpr std::uint64_t k_check_interval_ns = 250'000'000ULL;
constexpr std::uint64_t k_rate_limited_pause_ns = 60'000'000'000ULL;
constexpr std::size_t k_checks_in_flight = 4;

// a key is "<topic>\x1f<filter>"
[[nodiscard]] std::string frame_for(std::string_view key, std::size_t local_no) {
    const auto cut = key.find(k_key_sep);
    return R"({"SendTopicAction":{"Action":"1","FilterValue":")" + std::string{key.substr(cut + 1)} +
           R"(","LocalNo":)" + std::to_string(local_no) + R"(,"ResumeNo":-1,"TopicID":")" +
           std::string{key.substr(0, cut)} + R"("}})";
}

struct Found {
    const InstrumentBase* base;
    Market* market;
};

[[nodiscard]] std::optional<Found> lookup(VenueState& state, std::string_view symbol) noexcept {
    const auto* instrument = state.instruments.find(symbol);
    if (instrument == nullptr) {
        return std::nullopt;
    }
    const auto& base = base_of(*instrument);
    auto* market = state.market_of(base.id);
    if (market == nullptr) {
        return std::nullopt;
    }
    return Found{&base, market};
}

void publish(VenueState& state, const Found& found, std::uint64_t bno, UnixNanos received) {
    const auto book = found.market->ladders.snapshot(found.base->id, bno, UnixNanos{}, received);
    if (!book.has_value()) {
        return;
    }
    if (const auto top = top_of(*book); top.has_value()) {
        if (top->bid.raw >= top->ask.raw) {
            found.market->synced = false;
            ++state.book_faults;
            return;
        }
        state.sink->on_bbo(*top);
        found.market->priced = true;
    }
    state.sink->on_depth(*book);
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
        switch (head.action) {
        case Action::Book:
            on_book(payload, received);
            break;
        case Action::Trade:
            on_trades(payload, received);
            break;
        case Action::Overview:
            on_overview(payload, received);
            break;
        case Action::Ack:
            if (head.error_code != 0) {
                refused(payload);
            }
            break;
        case Action::Note:
        case Action::Unknown:
            break;
        }
    }

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    void refused(std::string_view payload) {
        if (++state_->subscribe_refusals <= 5 && ::raikiri::log::logger() != nullptr) {
            RLOG_WARN("lbank: subscription refused: {}", payload.substr(0, 240));
        }
    }

    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_book(payload, *storage);
        if (!wire.has_value()) {
            return;
        }
        if (wire->snapshot) {
            constexpr std::string_view k_prefix = "Exchange_";
            if (!wire->index.starts_with(k_prefix)) {
                return;
            }
            const auto symbol = wire->index.substr(k_prefix.size());
            const auto found = lookup(*state_, symbol);
            if (!found.has_value()) {
                return;
            }
            auto& market = *found->market;
            market.ladders.clear();
            market.synced = apply_levels(*wire, symbol, *found->base, market.ladders);
            market.last_bno = state_->max_bno;
            if (market.synced) {
                publish(*state_, *found, market.last_bno, received);
            }
            return;
        }
        if (!fresh_batch(wire->bno, state_->max_bno)) {
            return;
        }
        const auto frame = ++state_->frames;
        touched_.clear();
        std::string_view last{};
        std::optional<Found> found;
        for (const auto& level : wire->levels) {
            if (!level.tick.empty()) {
                continue;
            }
            if (level.instrument != last) {
                last = level.instrument;
                found = lookup(*state_, level.instrument);
                if (found.has_value() && !found->market->synced) {
                    found.reset();
                }
                if (found.has_value() && found->market->touched_frame != frame) {
                    found->market->touched_frame = frame;
                    touched_.push_back(*found);
                }
            }
            if (found.has_value() && !apply_level(level, *found->base, found->market->ladders)) {
                found->market->synced = false;
                ++state_->book_faults;
                found.reset();
            }
        }
        for (const auto& one : touched_) {
            if (one.market->synced) {
                one.market->last_bno = wire->bno;
                publish(*state_, one, wire->bno, received);
            }
        }
    }

    void on_trades(std::string_view payload, UnixNanos received) {
        static thread_local TradeBuffer storage;
        const auto rows = parse_trades(payload, storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(*state_, row.instrument);
            const auto id = wire::digits_to_u64(row.trade_id);
            if (!found.has_value() || id <= found->market->last_trade_id) {
                continue;
            }
            found->market->last_trade_id = id;
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    void on_overview(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<OverviewBuffer>();
        const auto rows = parse_overview(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(*state_, row.instrument);
            if (!found.has_value()) {
                continue;
            }
            if (const auto funding = map_funding(row, *found->base, found->market->interval_s, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                found->market->funded = true;
            }
            if (const auto stats = map_perp_stats(row, *found->base, received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    VenueState* state_;
    std::vector<Found> touched_;
};

class PerpData final : public exec::DataClient {
public:
    explicit PerpData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{
                    .host = std::string{ep::k_perp_ws_host}, .port = "443", .target = std::string{ep::k_perp_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { resync(keys); }},
          rest_{state_->io,
                net::RestConfig{
                    .host = std::string{ep::k_perp_rest_host}, .port = "443", .pool_size = k_checks_in_flight}} {
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
        const auto* market = state_->market_of(instrument);
        if (symbol.empty() || market == nullptr || instrument.market() != MarketType::Perp) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::string key;
        switch (kind) {
        case exec::SubKind::Bbo:
        case exec::SubKind::Depth:
            key = std::string{ep::k_topic_book} + k_key_sep + std::string{ep::k_exchange} + '_' + std::string{symbol} +
                  '_' + market->tick_text;
            break;
        case exec::SubKind::Trades:
            key = std::string{ep::k_topic_trades} + k_key_sep + std::string{ep::k_exchange} + '_' + std::string{symbol};
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            key = std::string{ep::k_topic_overview} + k_key_sep + std::string{ep::k_product_group};
            break;
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (!asked_.insert(key).second) {
            return {};
        }
        if (key.starts_with(ep::k_topic_book)) {
            state_->markets[instrument.symbol().raw].book = instrument;
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
        if (pool_.connected() && now.ns >= state_->next_app_ping_ns) {
            pool_.broadcast(ep::k_perp_ping);
            state_->next_app_ping_ns = now.ns + ep::k_app_ping_interval_ns;
        }
        if (in_flight_ < k_checks_in_flight && now.ns >= next_check_ns_) {
            check_next(now);
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
        std::vector<std::string> frames;
        frames.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            frames.push_back(frame_for(keys[i], i + 1));
        }
        return frames;
    }

    // a reopened socket replays a snapshot for each of its books
    void resync(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto last = key.rfind('_');
            const auto first = key.find('_');
            if (!key.starts_with(ep::k_topic_book) || last == first) {
                continue;
            }
            const auto symbol = std::string_view{key}.substr(first + 1, last - first - 1);
            if (const auto* instrument = state_->instruments.find(symbol); instrument != nullptr) {
                if (auto* market = state_->market_of(base_of(*instrument).id); market != nullptr) {
                    market->synced = false;
                }
            }
        }
    }

    // the diff stream drops rows, so every book is held against the rest touch in turn
    void check_next(UnixNanos now) {
        // every other turn goes to a broken book, so one the rest cannot seed never starves the round
        const auto count = state_->markets.size();
        const bool broken_turn = (++turns_ & 1U) != 0;
        auto& cursor = broken_turn ? broken_cursor_ : cursor_;
        std::size_t pick = count;
        for (std::size_t i = 1; i <= count && pick == count; ++i) {
            const auto at = (cursor + i) % count;
            const auto& market = state_->markets[at];
            if (market.book.packed != 0 && !market.checking && (!broken_turn || !market.synced)) {
                pick = at;
            }
        }
        if (pick == count) {
            return;
        }
        cursor = pick;
        const auto id = state_->markets[pick].book;
        target_ = std::string{ep::k_perp_depth} + std::string{state_->instruments.exchange_symbol(id)};
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target_, .body = {}};
        bool sent = false;
        try {
            sent = rest_.send(request, [this, id](net::RestResponse&& response) { on_depth(id, response); });
        } catch (...) {
            sent = false;
        }
        if (sent) {
            state_->markets[pick].checking = true;
            ++in_flight_;
            next_check_ns_ = now.ns + k_check_interval_ns;
        }
    }

    void on_depth(InstrumentId id, const net::RestResponse& response) {
        --in_flight_;
        auto* market = state_->market_of(id);
        if (market != nullptr) {
            market->checking = false;
        }
        if (response.status == 429) {
            ++refused_checks_;
            next_check_ns_ = state_->clock->wall().ns + k_rate_limited_pause_ns;
            return;
        }
        if (response.transport_failed() || response.status != 200) {
            return;
        }
        static thread_local const auto storage = std::make_unique<RestDepthBuffer>();
        const auto wire = parse_market_order(response.body, *storage);
        const auto* instrument = state_->instruments.find(id);
        if (!wire.has_value() || instrument == nullptr || market == nullptr) {
            return;
        }
        const Found found{&base_of(*instrument), market};
        ++checks_;
        if (market->synced && same_touch(market->ladders, *wire, *found.base)) {
            return;
        }
        ++(market->synced ? drifted_ : seeded_);
        market->synced = seed_levels(*wire, *found.base, market->ladders);
        if (market->synced) {
            publish(*state_, found, state_->max_bno, state_->clock->wall());
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
        RLOG_INFO("lbank: {} of {} contracts priced, {} traded, {} funded; {} sockets, {} keys, "
                  "{} refusals, {} book faults; {} rest checks, {} drifted, {} seeded, {} rate limited",
                  priced,
                  state_->markets.size(),
                  traded,
                  funded,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  state_->book_faults,
                  checks_,
                  drifted_,
                  seeded_,
                  refused_checks_);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    std::unordered_set<std::string> asked_;
    std::string target_;
    std::size_t cursor_{0};
    std::size_t broken_cursor_{0};
    std::uint64_t turns_{0};
    std::uint64_t next_check_ns_{0};
    std::uint64_t checks_{0};
    std::uint64_t drifted_{0};
    std::uint64_t seeded_{0};
    std::uint64_t refused_checks_{0};
    std::size_t in_flight_{0};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_perp_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<PerpData>(std::move(state));
}

} // namespace raikiri::venues::lbank::internal
