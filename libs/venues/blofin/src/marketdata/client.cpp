#include <raikiri/exec/events.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/blofin/book.hpp>
#include <raikiri/venues/blofin/endpoints.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/map.hpp>
#include <raikiri/venues/blofin/market_data.hpp>
#include <raikiri/venues/blofin/ws.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::blofin::internal {

namespace {

constexpr std::uint64_t k_funding_poll_interval_ns = 60ULL * 1'000'000'000ULL;
constexpr std::size_t k_refusals_logged = 20;

constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view channel, std::string_view inst_id) {
    std::string key{channel};
    key.push_back(k_key_sep);
    key.append(inst_id);
    return key;
}

struct Key {
    std::string_view channel;
    std::string_view inst_id;
};

[[nodiscard]] std::optional<Key> split_key(std::string_view key) {
    const auto cut = key.find(k_key_sep);
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    return Key{.channel = key.substr(0, cut), .inst_id = key.substr(cut + 1)};
}

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

class BlofinData final : public exec::DataClient {
public:
    explicit BlofinData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_public}},
                handler_,
                net::WsPoolConfig{},
                [this](std::span<const std::string> keys) { return build_frames(keys); },
                [this](std::span<const std::string> keys) { on_socket_open(keys); }},
          rest_{state_->io,
                net::RestConfig{.host = std::string{ep::host(state_->testnet)}, .port = "443", .pool_size = 2}} {
        rest_.set_admit(exec::market_data_gate(state_->budget, *state_->clock));
    }

    std::expected<void, exec::VenueError> start() override {
        rest_.start();
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report_final();
        pool_.stop();
        rest_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return refusals_; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind, InstrumentId instrument) override {
        const auto* listed = state_->instruments.find(instrument);
        if (listed == nullptr || instrument.market() != k_market) {
            return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto& base = base_of(*listed);
        const auto symbol = base.exchange_symbol.view();
        switch (kind) {
        case exec::SubKind::Bbo:
            quote_wanted_.insert(instrument.packed);
            return open(k_books_channel, symbol);
        case exec::SubKind::Depth:
            depth_wanted_.insert(instrument.packed);
            return open(k_books_channel, symbol);
        case exec::SubKind::Trades:
            return open(k_trades_channel, symbol);
        case exec::SubKind::Funding:
        case exec::SubKind::MarkPrice: {
            if (funding_wanted_.insert(instrument.packed).second) {
                memos_[instrument.packed].interval_s = std::get<PerpInstrument>(*listed).funding_interval_s;
            }
            if (const auto opened = open(k_funding_channel, symbol); !opened.has_value()) {
                return opened;
            }
            return open(k_tickers_channel, symbol);
        }
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
        if (pool_.connected() && now.ns >= next_ping_ns_) {
            pool_.broadcast(Protocol::ping_frame());
            next_ping_ns_ = now.ns + k_ping_interval_ns;
        }
        poll_funding(now);
    }

private:
    // perp only: spot frames carry the same instId and no market marker
    static constexpr MarketType k_market = MarketType::Perp;

    class Handler final : public net::WsHandler {
    public:
        explicit Handler(BlofinData& owner) : owner_{&owner} {}

        void on_open() override {
            owner_->state_->sink->on_connectivity(
                {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = owner_->state_->clock->wall()});
        }

        void on_message(std::string_view payload, UnixNanos received) noexcept override {
            owner_->on_frame(payload, received);
        }

        void on_close(net::WsCloseReason) override {
            owner_->state_->sink->on_connectivity(
                {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = owner_->state_->clock->wall()});
        }

    private:
        BlofinData* owner_;
    };

    [[nodiscard]] std::expected<void, exec::VenueError> open(std::string_view channel, std::string_view inst_id) {
        auto key = key_of(channel, inst_id);
        if (!desired_.insert(key).second) {
            return {};
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            desired_.erase(one[0]);
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "blofin: connection pool is full")};
        }
        return {};
    }

    [[nodiscard]] const Instrument* find(std::string_view inst_id) const noexcept {
        return state_->instruments.find(inst_id, k_market);
    }

    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string_view> order;
        std::unordered_map<std::string_view, std::vector<std::string_view>> channels;
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value()) {
                continue;
            }
            auto [entry, fresh] = channels.try_emplace(one->inst_id);
            if (fresh) {
                order.push_back(one->inst_id);
            }
            entry->second.push_back(one->channel);
        }
        std::vector<std::string> frames;
        frames.reserve(order.size());
        for (const auto inst_id : order) {
            if (const auto frame = protocol_.subscribe_frame(inst_id, channels[inst_id]); frame.has_value()) {
                frames.emplace_back(*frame);
            }
        }
        return frames;
    }

    void on_socket_open(std::span<const std::string> keys) {
        for (const auto& key : keys) {
            const auto one = split_key(key);
            if (!one.has_value() || one->channel != k_books_channel) {
                continue;
            }
            const auto* instrument = find(one->inst_id);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto book = books_.find(base_of(*instrument).id.packed); book != books_.end()) {
                book->second.invalidate();
            }
        }
    }

    void on_frame(std::string_view frame, UnixNanos received) noexcept {
        const auto event = parse_event(frame);
        if (!event.has_value()) {
            return;
        }
        switch (event->kind) {
        case EventKind::Books:
            on_books(frame, received);
            break;
        case EventKind::Trades:
            on_trades(frame, received);
            break;
        case EventKind::Tickers:
            on_tickers(frame, received);
            break;
        case EventKind::FundingRate:
            on_funding_frame(frame, received);
            break;
        case EventKind::Error:
            on_error(frame);
            break;
        case EventKind::SubscribeAck:
        case EventKind::Unknown:
            break;
        }
    }

    void on_books(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<LevelBuffer>();
        const auto wire = parse_books(frame, *storage);
        if (!wire.has_value()) {
            ++unreadable_;
            return;
        }
        const auto* instrument = find(wire->inst_id);
        if (instrument == nullptr) {
            return;
        }
        const auto& base = base_of(*instrument);
        auto& book = books_.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first->second;
        const auto result = book.apply(*wire);
        if (result == BookResult::Gap) {
            resubscribe_book(wire->inst_id);
            return;
        }
        if (result != BookResult::Applied && result != BookResult::Resynced) {
            return;
        }
        const auto ts_event = ms_to_unix(wire->ts_ms, received);
        if (quote_wanted_.contains(base.id.packed)) {
            if (const auto top = book.top(ts_event, received); top.has_value()) {
                state_->sink->on_bbo(*top);
                quoted_.insert(base.id.packed);
            }
        }
        if (!depth_wanted_.contains(base.id.packed) || !state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto depth = book.snapshot(ts_event, received); depth.has_value()) {
            state_->sink->on_depth(*depth);
        }
    }

    // the venue sends a snapshot only on a fresh subscription; the book answers Ignored until then
    void resubscribe_book(std::string_view inst_id) noexcept {
        const std::array<std::string_view, 1> books{k_books_channel};
        const auto off = protocol_.unsubscribe_frame(inst_id, books);
        if (!off.has_value() || !pool_.reply(*off)) {
            return;
        }
        const auto on = protocol_.subscribe_frame(inst_id, books);
        if (on.has_value() && pool_.reply(*on)) {
            ++resubscribes_;
        }
    }

    void on_trades(std::string_view frame, UnixNanos received) noexcept {
        static thread_local PrintBuffer storage;
        const auto prints = parse_trades(frame, storage);
        if (!prints.has_value()) {
            return;
        }
        for (const auto& print : *prints) {
            const auto* instrument = find(print.inst_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            if (const auto trade = map_trade(print, base, received); trade.has_value()) {
                state_->sink->on_trade(*trade);
                traded_.insert(base.id.packed);
            }
        }
    }

    void on_tickers(std::string_view frame, UnixNanos received) noexcept {
        static thread_local TickerBuffer storage;
        const auto rows = parse_tickers(frame, storage);
        if (!rows.has_value()) {
            return;
        }
        for (const auto& row : *rows) {
            const auto* instrument = find(row.inst_id);
            if (instrument == nullptr) {
                continue;
            }
            if (const auto stats = map_ticker_stats(row, base_of(*instrument), received); stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
        }
    }

    void on_funding_frame(std::string_view frame, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<FundingBuffer>();
        const auto rows = parse_funding(frame, *storage);
        if (rows.has_value()) {
            note_rates(*rows, received, received);
        }
    }

    void note_rates(std::span<const WireFundingRow> rows, UnixNanos ts_event, UnixNanos received) noexcept {
        for (const auto& row : rows) {
            const auto* instrument = find(row.inst_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            const auto memo = memos_.find(base.id.packed);
            if (memo == memos_.end() || !note_funding(memo->second, row)) {
                continue;
            }
            state_->sink->on_funding(merged(base, memo->second, ts_event, received));
            funded_.insert(base.id.packed);
        }
    }

    void on_error(std::string_view frame) noexcept {
        const auto err = parse_stream_error(frame);
        if (!err.has_value()) {
            ++refusals_;
            return;
        }
        refusals_ += err->args == 0 ? 1 : err->args;
        last_error_code_ = err->code;
        if (!err->inst_id.empty()) {
            refused_.emplace(err->inst_id);
        }
        if (++refusals_logged_ <= k_refusals_logged && has_logger()) {
            RLOG_WARN("blofin: the venue refused {} subscription(s) for {} — code {}",
                      err->args,
                      err->inst_id.empty() ? std::string_view{"an unnamed instrument"} : err->inst_id,
                      err->code);
        }
    }

    // the venue changes the period on live contracts, so it is re-read every pass
    void poll_funding(UnixNanos now) noexcept {
        if (funding_wanted_.empty() || now.ns < next_poll_ns_ || rates_in_flight_ || marks_in_flight_) {
            return;
        }
        const net::RestRequest rates{.method = net::HttpMethod::Get, .target = ep::k_funding_rate, .body = {}};
        const net::RestRequest marks{.method = net::HttpMethod::Get, .target = ep::k_mark_price, .body = {}};
        try {
            rates_in_flight_ = rest_.send(rates, [this](net::RestResponse&& response) { on_rate_table(response); });
            marks_in_flight_ = rest_.send(marks, [this](net::RestResponse&& response) { on_mark_table(response); });
        } catch (...) {
            return;
        }
        if (rates_in_flight_ && marks_in_flight_) {
            next_poll_ns_ = now.ns + k_funding_poll_interval_ns;
        }
    }

    [[nodiscard]] bool answered(const net::RestResponse& response) noexcept {
        if (response.status == 429 || response.status == 403) {
            next_poll_ns_ = state_->clock->wall().ns + ep::k_rest_suspension_ns;
            if (has_logger()) {
                RLOG_WARN("blofin: REST answered {} — funding polls pause for five minutes", response.status);
            }
            return false;
        }
        if (response.transport_failed() || response.status != 200) {
            return false;
        }
        const auto envelope = parse_rest_envelope(response.body);
        return envelope.has_value() && envelope->code == k_ok;
    }

    void on_rate_table(const net::RestResponse& response) noexcept {
        rates_in_flight_ = false;
        if (!answered(response)) {
            return;
        }
        static thread_local const auto storage = std::make_unique<FundingBuffer>();
        const auto rows = parse_funding(response.body, *storage);
        if (rows.has_value()) {
            note_rates(*rows, response.received, state_->clock->wall());
        }
    }

    void on_mark_table(const net::RestResponse& response) noexcept {
        marks_in_flight_ = false;
        if (!answered(response)) {
            return;
        }
        static thread_local const auto storage = std::make_unique<MarkBuffer>();
        const auto rows = parse_mark_table(response.body, *storage);
        if (!rows.has_value()) {
            return;
        }
        const auto received = state_->clock->wall();
        for (const auto& row : *rows) {
            const auto* instrument = find(row.inst_id);
            if (instrument == nullptr) {
                continue;
            }
            const auto& base = base_of(*instrument);
            const auto memo = memos_.find(base.id.packed);
            if (memo == memos_.end() || !note_mark(memo->second, row, base) || memo->second.rate.is_undef()) {
                continue;
            }
            state_->sink->on_funding(merged(base, memo->second, ms_to_unix(row.ts_ms, received), received));
        }
    }

    void report_final() const noexcept {
        if (!has_logger()) {
            return;
        }
        std::uint64_t gaps = 0;
        std::size_t live = 0;
        for (const auto& [packed, book] : books_) {
            gaps += book.gaps();
            live += book.synced() ? 1U : 0U;
        }
        std::map<std::uint32_t, std::size_t> periods;
        for (const auto packed : funded_) {
            ++periods[memos_.at(packed).interval_s];
        }
        std::string spread;
        for (const auto& [seconds, count] : periods) {
            spread += seconds == 0          ? std::string{" unstated"}
                      : seconds % 3600 == 0 ? " " + std::to_string(seconds / 3600) + "h"
                                            : " " + std::to_string(seconds / 60) + "m";
            spread += "=" + std::to_string(count);
        }
        RLOG_INFO("blofin: {} of {} perps quoted, {} of {} depth books live, {} traded, {} funded "
                  "(periods{}) — {} chain gap(s), {} resubscribe(s), {} unreadable book frame(s)",
                  quoted_.size(),
                  quote_wanted_.size(),
                  live,
                  books_.size(),
                  traded_.size(),
                  funded_.size(),
                  spread,
                  gaps,
                  resubscribes_,
                  unreadable_);
        if (refusals_ != 0) {
            RLOG_WARN("blofin: the venue refused {} subscription(s) across {} instrument(s), the "
                      "last with code {}",
                      refusals_,
                      refused_.size(),
                      last_error_code_);
        }
    }

    std::shared_ptr<VenueState> state_;
    Handler handler_;
    net::WsPool pool_;
    net::RestClient rest_;
    Protocol protocol_;

    std::unordered_set<std::string> desired_;
    std::unordered_map<std::uint64_t, Book> books_;
    std::unordered_map<std::uint64_t, FundingMemo> memos_;
    std::unordered_set<std::uint64_t> quote_wanted_;
    std::unordered_set<std::uint64_t> depth_wanted_;
    std::unordered_set<std::uint64_t> funding_wanted_;

    std::unordered_set<std::uint64_t> quoted_;
    std::unordered_set<std::uint64_t> traded_;
    std::unordered_set<std::uint64_t> funded_;
    std::unordered_set<std::string> refused_;
    std::uint64_t refusals_{0};
    std::uint64_t refusals_logged_{0};
    std::int64_t last_error_code_{0};
    std::uint64_t resubscribes_{0};
    std::uint64_t unreadable_{0};

    std::uint64_t next_ping_ns_{0};
    std::uint64_t next_poll_ns_{0};
    bool rates_in_flight_{false};
    bool marks_in_flight_{false};
    bool started_{false};
};

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<BlofinData>(std::move(state));
}

} // namespace raikiri::venues::blofin::internal
