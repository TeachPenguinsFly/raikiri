#include <raikiri/exec/events.hpp>
#include <raikiri/exec/ticker_poll.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/upbit/endpoints.hpp>
#include <raikiri/venues/upbit/map.hpp>
#include <raikiri/venues/upbit/wire.hpp>

#include <array>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::upbit::internal {

namespace {

constexpr std::uint64_t k_report_interval_ns = 60'000'000'000ULL;
constexpr std::string_view k_orderbook = "orderbook";
constexpr std::string_view k_depth = "depth";
constexpr std::string_view k_trade = "trade";
constexpr std::string_view k_depth_units = ".15";
constexpr std::size_t k_depth_levels = 15;

std::string key_of(std::string_view type, std::string_view code) {
    return std::format("{}|{}", type, code);
}

class Handler final : public net::WsHandler {
public:
    explicit Handler(VenueState& state) : state_{&state} {}

    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override {
        switch (parse_kind(payload)) {
        case Kind::Orderbook:
            on_book(payload, received);
            break;
        case Kind::Trade:
            on_trade(payload, received);
            break;
        case Kind::Refusal:
            ++state_->subscribe_refusals;
            break;
        case Kind::Unknown:
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

    [[nodiscard]] std::optional<Found> lookup(std::string_view code) noexcept {
        const auto* instrument = state_->instruments.find(code);
        if (instrument == nullptr) {
            return std::nullopt;
        }
        const auto& base = base_of(*instrument);
        auto* market = state_->market_of(base.id);
        return market == nullptr ? std::nullopt : std::optional{Found{&base, market}};
    }

    // each frame is the whole top of the book, so one older than the last is dropped, not merged
    void on_book(std::string_view payload, UnixNanos received) {
        static thread_local LevelBuffer storage;
        const auto wire = parse_book(payload, storage);
        if (!wire.has_value()) {
            return;
        }
        const auto found = lookup(wire->code);
        if (!found.has_value() || !found->market->booked) {
            return;
        }
        if (wire->ts < found->market->last_book_ts) {
            ++state_->stale_books;
            return;
        }
        const auto book = map_book(*wire, *found->base, received);
        if (!book.has_value()) {
            ++state_->refused_books;
            return;
        }
        found->market->last_book_ts = wire->ts;
        if (book->bid_count > 0 && book->ask_count > 0) {
            state_->sink->on_bbo(Bbo{.instrument = book->instrument,
                                     .bid = book->bids[0].price,
                                     .ask = book->asks[0].price,
                                     .bid_qty = book->bids[0].qty,
                                     .ask_qty = book->asks[0].qty,
                                     .ts_event = book->ts_event,
                                     .ts_init = received});
            found->market->priced = true;
        }
        if (found->market->depth && wire->bids.size() >= k_depth_levels && book->bid_count + book->ask_count > 0 &&
            state_->sink->wants_depth(book->instrument, received)) {
            state_->sink->on_depth(*book);
        }
    }

    // the print a subscription opens with is already history
    void on_trade(std::string_view payload, UnixNanos received) {
        const auto wire = parse_trade(payload);
        if (!wire.has_value() || wire->snapshot) {
            return;
        }
        const auto found = lookup(wire->code);
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

class UpbitData final : public exec::DataClient {
public:
    explicit UpbitData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_},
          pool_{state_->io,
                net::WsConfig{.host = std::string{state_->flavour.ws_host},
                              .port = "443",
                              .target = std::string{state_->flavour.ws_path}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); }},
          rest_{state_->io,
                net::RestConfig{.host = std::string{state_->flavour.rest_host}, .port = "443", .timeout_ms = 20'000}} {
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
        const auto code = state_->instruments.exchange_symbol(instrument);
        auto* market = state_->market_of(instrument);
        if (code.empty() || market == nullptr) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        std::string key;
        switch (kind) {
        case exec::SubKind::Bbo:
            if (!volumes_.wanted(instrument)) {
                volumes_.want(instrument);
                ticker_codes_.emplace_back(code);
                targets_dirty_ = true;
            }
            market->booked = true;
            key = key_of(k_orderbook, code);
            break;
        case exec::SubKind::Depth:
            market->depth = true;
            market->booked = true;
            key = key_of(k_depth, code);
            break;
        case exec::SubKind::Trades:
            key = key_of(k_trade, code);
            break;
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice:
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        if (!subscribed_.insert(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "connection pool is full")};
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
        if (next_chunk_ != 0 && !chunk_in_flight_) {
            send_chunk();
        }
        volumes_.poll(now, [this] {
            if (targets_dirty_) {
                rebuild_targets();
            }
            next_chunk_ = 0;
            return send_chunk();
        });
        if (now.ns >= next_report_ns_) {
            if (next_report_ns_ != 0) {
                report();
            }
            next_report_ns_ = now.ns + k_report_interval_ns;
        }
    }

private:
    // a request replaces whatever the socket carried, so each frame names every key on it
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::unordered_set<std::string_view> deep;
        for (const auto& key : keys) {
            if (key.starts_with(k_depth)) {
                deep.insert(std::string_view{key}.substr(k_depth.size() + 1));
            }
        }
        std::string books;
        std::string trades;
        const auto add = [](std::string& list, std::string_view code, std::string_view units) {
            list += list.empty() ? "\"" : ",\"";
            list += code;
            list += units;
            list += '"';
        };
        for (const auto& key : keys) {
            const auto bar = key.find('|');
            const std::string_view type{key.data(), bar};
            const auto code = std::string_view{key}.substr(bar + 1);
            if (type == k_trade) {
                add(trades, code, {});
            } else if (type == k_depth) {
                add(books, code, k_depth_units);
            } else if (!deep.contains(code)) {
                add(books, code, state_->flavour.quote_units);
            }
        }
        std::string frame = std::format(R"([{{"ticket":"raikiri-{}"}})", ++next_ticket_);
        if (!books.empty()) {
            frame += std::format(R"(,{{"type":"orderbook","codes":[{}]}})", books);
        }
        if (!trades.empty()) {
            frame += std::format(R"(,{{"type":"trade","codes":[{}]}})", trades);
        }
        frame += R"(,{"format":"DEFAULT"}])";
        return {std::move(frame)};
    }

    void rebuild_targets() {
        targets_dirty_ = false;
        targets_.clear();
        if (!state_->flavour.ticker_all.empty()) {
            targets_.emplace_back(state_->flavour.ticker_all);
            return;
        }
        const auto chunk = state_->flavour.ticker_chunk;
        for (std::size_t at = 0; at < ticker_codes_.size(); at += chunk) {
            std::string target{ep::k_ticker};
            for (std::size_t i = at; i < ticker_codes_.size() && i < at + chunk; ++i) {
                target += i == at ? "" : ",";
                target += ticker_codes_[i];
            }
            targets_.push_back(std::move(target));
        }
    }

    // the cycle walks every list; a refused send ends it until the next interval
    bool send_chunk() {
        if (next_chunk_ >= targets_.size()) {
            finish_cycle();
            return false;
        }
        const net::RestRequest request{.method = net::HttpMethod::Get, .target = targets_[next_chunk_]};
        chunk_in_flight_ = rest_.send(request, [this](const net::RestResponse& response) { on_tickers(response); });
        if (!chunk_in_flight_ && next_chunk_ != 0) {
            finish_cycle();
        }
        return chunk_in_flight_;
    }

    void finish_cycle() {
        next_chunk_ = 0;
        chunk_in_flight_ = false;
        volumes_.answered();
    }

    void on_tickers(const net::RestResponse& response) noexcept {
        chunk_in_flight_ = false;
        ++next_chunk_;
        if (!response.transport_failed() && response.status == 200) {
            static thread_local const auto storage = std::make_unique<TickerBuffer>();
            if (const auto rows = parse_tickers(response.body, *storage); rows.has_value()) {
                emit(*rows);
            }
        }
        if (next_chunk_ >= targets_.size()) {
            finish_cycle();
        }
    }

    void emit(std::span<const WireTicker> rows) {
        const auto received = state_->clock->wall();
        for (const auto& row : rows) {
            const auto* instrument = state_->instruments.find(row.market);
            if (instrument == nullptr || !volumes_.wanted(base_of(*instrument).id)) {
                continue;
            }
            const auto& base = base_of(*instrument);
            const auto stats = map_ticker_stats(row, base, state_->flavour.ticker_stamped, received);
            if (!stats.has_value()) {
                continue;
            }
            state_->sink->on_ticker_stats(*stats);
            if (auto* market = state_->market_of(base.id); market != nullptr) {
                market->volumed = true;
            }
        }
    }

    void report() const {
        if (::raikiri::log::logger() == nullptr) {
            return;
        }
        std::size_t priced = 0;
        std::size_t traded = 0;
        std::size_t volumed = 0;
        for (const auto& market : state_->markets) {
            priced += market.priced ? 1U : 0U;
            traded += market.traded ? 1U : 0U;
            volumed += market.volumed ? 1U : 0U;
        }
        RLOG_INFO("{}: {} of {} markets priced two-sided, {} traded, {} with volume; {} sockets, {} keys, "
                  "{} refusals, {} stale and {} refused books",
                  state_->flavour.name,
                  priced,
                  state_->markets.size(),
                  traded,
                  volumed,
                  pool_.connection_count(),
                  pool_.key_count(),
                  state_->subscribe_refusals,
                  state_->stale_books,
                  state_->refused_books);
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    exec::TickerPoll volumes_;
    std::unordered_set<std::string> subscribed_;
    std::vector<std::string> ticker_codes_;
    std::vector<std::string> targets_;
    std::size_t next_chunk_{0};
    bool chunk_in_flight_{false};
    bool targets_dirty_{false};
    bool started_{false};
    std::uint64_t next_ticket_{0};
    std::uint64_t next_report_ns_{0};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<UpbitData>(std::move(state));
}

} // namespace raikiri::venues::upbit::internal
