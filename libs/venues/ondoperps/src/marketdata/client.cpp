#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/ondoperps/endpoints.hpp>
#include <raikiri/venues/ondoperps/map.hpp>
#include <raikiri/venues/ondoperps/wire.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::ondoperps::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::uint64_t k_retry_ns = 1'000'000'000ULL;
constexpr char k_key_sep = '|';

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_frame(payload)) {
        case Frame::Top:
        case Frame::Depth:
            on_books(payload, received);
            break;
        case Frame::Trades:
            on_trades(payload, received);
            break;
        case Frame::Marks:
            on_marks(payload);
            break;
        case Frame::Funding:
            on_funding(payload, received);
            break;
        case Frame::Error:
            ++state_->subscribe_refusals;
            break;
        case Frame::Subscribed:
        case Frame::Pong:
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

    void on_books(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<BookBuffer>();
        const bool top = parse_frame(payload) == Frame::Top;
        const auto rows = parse_books(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.market);
            if (!found.has_value()) {
                continue;
            }
            if (top) {
                if (const auto bbo = map_top(row, *found->base, received); bbo.has_value() && found->market->bbo) {
                    state_->sink->on_bbo(*bbo);
                    found->market->priced = true;
                }
            } else if (found->market->depth) {
                if (const auto book = map_book(row, *found->base, received); book.has_value()) {
                    state_->sink->on_depth(*book);
                }
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
            const auto found = lookup(row.market);
            if (!found.has_value()) {
                continue;
            }
            if (const auto trade = map_trade(row, *found->base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                found->market->traded = true;
            }
        }
    }

    void on_marks(std::string_view payload) {
        static thread_local const auto storage = std::make_unique<MarkBuffer>();
        const auto rows = parse_marks(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.market);
            if (!found.has_value()) {
                continue;
            }
            if (const auto mark = mark_ticks(row, *found->base); mark.has_value()) {
                found->market->mark = *mark;
            }
            if (const auto index = oracle_ticks(row, *found->base); index.has_value()) {
                found->market->index = *index;
            }
        }
    }

    void on_funding(std::string_view payload, UnixNanos received) {
        static thread_local const auto storage = std::make_unique<FundingBuffer>();
        const auto rows = parse_funding(payload, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto found = lookup(row.market);
            if (!found.has_value() || !found->market->funding) {
                continue;
            }
            if (const auto funding = map_funding(
                    row, *found->base, found->market->mark, found->market->index, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
                found->market->funded = true;
            }
        }
    }

    VenueState* state_;
};

class OndoData final : public exec::DataClient {
public:
    explicit OndoData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::k_host}, .port = "443", .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{},
                [](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io, net::RestConfig{.host = std::string{ep::k_host}, .port = "443", .pool_size = 2}} {
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
        std::string key;
        switch (kind) {
        case exec::SubKind::Bbo:
            market->bbo = true;
            key = std::string{ep::k_top};
            break;
        case exec::SubKind::Depth:
            market->depth = true;
            key = std::string{ep::k_depth};
            break;
        case exec::SubKind::Trades:
            market->trades = true;
            key = std::string{ep::k_trades};
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
            market->funding = true;
            if (all_asked_) {
                return {};
            }
            all_asked_ = true;
            return place({std::string{ep::k_marks}, std::string{ep::k_funding}});
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        key.push_back(k_key_sep);
        key.append(symbol);
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
        if (now.ns >= next_stats_ns_ && !interest_in_flight_ && !volume_in_flight_) {
            interest_in_flight_ = ask(ep::k_open_interest, &OndoData::on_interest, interest_in_flight_);
            volume_in_flight_ = ask(ep::k_volume, &OndoData::on_volumes, volume_in_flight_);
            next_stats_ns_ = now.ns + (interest_in_flight_ && volume_in_flight_ ? ep::k_stats_interval_ns : k_retry_ns);
        }
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    using OnBody = void (OndoData::*)(std::string_view);

    std::expected<void, exec::VenueError> place(std::vector<std::string> keys) {
        if (!pool_.subscribe(keys)) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "ondoperps: connection pool is full")};
        }
        return {};
    }

    bool ask(std::string_view target, OnBody on_body, bool& in_flight) {
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = target, .body = {}};
        return rest_.send(request, [this, on_body, &in_flight](net::RestResponse&& response) {
            in_flight = false;
            if (response.status == 200) {
                (this->*on_body)(response.body);
            }
        });
    }

    [[nodiscard]] static std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::map<std::string, std::vector<std::string>> by_channel;
        for (const auto& key : keys) {
            const auto cut = key.find(k_key_sep);
            auto& markets = by_channel[key.substr(0, cut)];
            if (cut != std::string::npos) {
                markets.push_back(key.substr(cut + 1));
            }
        }
        std::vector<std::string> frames;
        for (const auto& [channel, markets] : by_channel) {
            std::string frame = R"({"op":"subscribe","channel":")" + channel + '"';
            if (!markets.empty()) {
                frame += R"(,"markets":[)";
                for (std::size_t i = 0; i < markets.size(); ++i) {
                    frame += i == 0 ? "\"" : ",\"";
                    frame += markets[i];
                    frame += '"';
                }
                frame += ']';
            }
            frame += '}';
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    [[nodiscard]] Market* find(std::string_view symbol) const {
        const auto* instrument = state_->instruments.find(symbol);
        return instrument != nullptr ? state_->market_of(base_of(*instrument).id) : nullptr;
    }

    void on_interest(std::string_view body) {
        static thread_local const auto storage = std::make_unique<StatBuffer>();
        const auto rows = parse_open_interest(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            if (auto* market = find(row.market); market != nullptr) {
                market->stats.open_interest = parse_amount_e9(row.first).value_or(FixedE9{});
            }
        }
        publish_stats();
    }

    void on_volumes(std::string_view body) {
        static thread_local const auto storage = std::make_unique<StatBuffer>();
        const auto rows = parse_volumes(body, *storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            if (auto* market = find(row.market); market != nullptr) {
                market->stats.volume_base = parse_amount_e9(row.first).value_or(FixedE9{});
                market->stats.volume_quote = parse_turnover_e6(row.second).value_or(FixedE6{});
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
                InstrumentId::make(VenueId::OndoPerps, MarketType::Perp, SymbolId{static_cast<std::uint32_t>(i)}));
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
        RLOG_INFO("ondoperps: {} of {} markets priced, {} traded, {} funded; {} sockets, {} keys, {} refusals",
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
    bool interest_in_flight_{false};
    bool volume_in_flight_{false};
    std::uint64_t next_app_ping_ns_{0};
    std::uint64_t next_stats_ns_{0};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<OndoData>(std::move(state));
}

} // namespace raikiri::venues::ondoperps::internal
