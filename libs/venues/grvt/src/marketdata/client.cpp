#include <raikiri/log/log.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/net/ws_pool.hpp>
#include <raikiri/venues/grvt/book.hpp>
#include <raikiri/venues/grvt/endpoints.hpp>
#include <raikiri/venues/grvt/map.hpp>
#include <raikiri/venues/grvt/wire.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../clients.hpp"

namespace raikiri::venues::grvt::detail {

namespace {

// A pool key is one selector on one stream, joined by a byte neither can
// contain. This file is the only reader of the shape.
constexpr char k_key_sep = '\x1f';

[[nodiscard]] std::string key_of(std::string_view stream, std::string_view selector) {
    std::string key{stream};
    key.push_back(k_key_sep);
    key.append(selector);
    return key;
}

// The quote first: once sends are paced, whatever goes last is what a
// half-subscribed socket is missing, and the quote is the column every
// screen reads.
constexpr std::array<std::string_view, 4> k_stream_order{
    ep::k_quote_stream, ep::k_trade_stream, ep::k_book_stream, ep::k_ticker_stream};

[[nodiscard]] bool states_top(const WireQuote& quote) noexcept {
    return quote.bid_price.has_value() || quote.bid_size.has_value() ||
           quote.ask_price.has_value() || quote.ask_size.has_value();
}

[[nodiscard]] bool has_logger() noexcept {
    return ::raikiri::log::logger() != nullptr;
}

class GrvtData;

class PublicHandler final : public net::WsHandler {
public:
    PublicHandler(VenueState& state, GrvtData& owner) : state_{&state}, owner_{&owner} {}

    // Nothing is resubscribed here: each socket in the pool re-asks for its own
    // keys when it opens, and every stream answers that with a fresh snapshot.
    void on_open() override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Resubscribed, .stream_id = 0, .ts = state_->clock->wall()});
    }

    void on_message(std::string_view payload, UnixNanos received) noexcept override;

    void on_close(net::WsCloseReason) override {
        state_->sink->on_connectivity(
            {.state = exec::ConnState::Disconnected, .stream_id = 0, .ts = state_->clock->wall()});
    }

private:
    VenueState* state_;
    GrvtData* owner_;
};

class GrvtData final : public exec::DataClient {
public:
    explicit GrvtData(std::shared_ptr<VenueState> state)
        : state_{std::move(state)}, handler_{*state_, *this},
          pool_{state_->io,
                net::WsConfig{.host = std::string{ep::host(state_->testnet)},
                              .port = "443",
                              .target = std::string{ep::k_ws_path}},
                handler_,
                net::WsPoolConfig{.max_keys_per_connection = ep::k_max_keys_per_connection,
                                  .send_spacing_ns = ep::k_subscribe_spacing_ns,
                                  .max_connections = ep::k_max_connections},
                [this](std::span<const std::string> keys) { return build_frames(keys); }} {}

    std::expected<void, exec::VenueError> start() override {
        pool_.start();
        started_ = true;
        return {};
    }

    void stop() override {
        report();
        pool_.stop();
        started_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override { return pool_.connected(); }

    [[nodiscard]] std::uint64_t subscribe_refusals() const noexcept override { return refusals_; }

    std::expected<void, exec::VenueError> subscribe(exec::SubKind kind,
                                                    InstrumentId instrument) override {
        const auto* listed = state_->instruments.find(instrument);
        if (listed == nullptr) {
            return std::unexpected{
                exec::VenueError::make(exec::ErrorClass::Terminal, 0, "unknown instrument")};
        }
        const auto& base = base_of(*listed);
        std::string_view stream;
        std::string_view rate;
        switch (kind) {
        case exec::SubKind::Bbo:
            stream = ep::k_quote_stream;
            rate = ep::k_quote_rate;
            quotes_.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size);
            break;
        case exec::SubKind::Trades:
            stream = ep::k_trade_stream;
            rate = ep::k_trade_rate;
            break;
        case exec::SubKind::Depth:
            stream = ep::k_book_stream;
            rate = ep::k_book_rate;
            books_.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size);
            break;
        case exec::SubKind::Funding:
            stream = ep::k_ticker_stream;
            rate = ep::k_ticker_rate;
            break;
        case exec::SubKind::MarkPrice:
        case exec::SubKind::Kline:
            return DataClient::subscribe(kind, instrument);
        }
        std::string selector{base.exchange_symbol.view()};
        selector.append(rate);
        auto key = key_of(stream, selector);
        if (!desired_.insert(key).second) {
            return {}; // already placed on a socket; a key is asked for once
        }
        const std::array<std::string, 1> one{std::move(key)};
        if (!pool_.subscribe(one)) {
            return std::unexpected{exec::VenueError::make(
                exec::ErrorClass::Terminal, 0, "grvt: connection pool is full")};
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
    }

    void on_frame(std::string_view payload, UnixNanos received) noexcept {
        static thread_local const auto storage = std::make_unique<MessageBuffer>();
        const auto message = parse_message(payload, *storage);
        if (!message.has_value()) {
            ++unreadable_;
            return;
        }
        switch (message->kind) {
        case MessageKind::Error:
            ++refusals_;
            last_error_code_ = message->error.code;
            break;
        case MessageKind::Ack:
            on_ack(message->ack);
            break;
        case MessageKind::Feed:
            on_feed(message->feed, received);
            break;
        }
    }

private:
    [[nodiscard]] const InstrumentBase* base_for(std::string_view selector) const noexcept {
        const auto* listed = state_->instruments.find(selector_instrument(selector));
        return listed == nullptr ? nullptr : &base_of(*listed);
    }

    void on_ack(const WireAck& ack) noexcept {
        for (std::size_t i = 0; i < ack.subs.size(); ++i) {
            const auto* base = base_for(ack.subs[i]);
            if (base == nullptr) {
                continue;
            }
            if (ack.stream == ep::k_quote_stream) {
                if (const auto found = quotes_.find(base->id.packed); found != quotes_.end()) {
                    found->second.sequence.acknowledged(ack.latest[i]);
                }
            } else if (ack.stream == ep::k_book_stream) {
                if (const auto found = books_.find(base->id.packed); found != books_.end()) {
                    found->second.sequence.acknowledged(ack.latest[i]);
                }
            }
        }
    }

    void on_feed(const WireFeed& feed, UnixNanos received) noexcept {
        const auto* base = base_for(feed.selector);
        if (base == nullptr) {
            return;
        }
        const auto ts_event = ns_or(feed.event_ns, received);
        switch (feed.kind) {
        case FeedKind::Quote:
            on_quote(feed, *base, ts_event, received);
            break;
        case FeedKind::Book:
            on_book(feed, *base, ts_event, received);
            break;
        case FeedKind::Trade:
            // The snapshot is the last 50 prints, replayed on every subscribe
            // and every reopen: history, not trading.
            if (feed.sequence == 0) {
                return;
            }
            if (const auto trade = map_trade(feed.print, *base, ts_event, received);
                trade.has_value()) {
                state_->sink->on_trade(*trade);
            }
            break;
        case FeedKind::Ticker: {
            const auto* listed = state_->instruments.find(base->id);
            if (listed == nullptr) {
                return;
            }
            if (const auto funding = map_funding(feed.ticker, *listed, ts_event, received);
                funding.has_value()) {
                state_->sink->on_funding(*funding);
            }
            if (const auto stats = map_ticker_stats(feed.ticker, *base, ts_event, received);
                stats.has_value()) {
                state_->sink->on_ticker_stats(*stats);
            }
            break;
        }
        case FeedKind::Unknown:
            break;
        }
    }

    void on_quote(const WireFeed& feed,
                  const InstrumentBase& base,
                  UnixNanos ts_event,
                  UnixNanos received) noexcept {
        const auto found = quotes_.find(base.id.packed);
        if (found == quotes_.end()) {
            return;
        }
        auto& quote = found->second;
        bool ok = false;
        switch (quote.sequence.offer(feed.sequence, feed.prev_sequence)) {
        case Step::Snapshot:
            ok = quote.replace(feed.quote);
            break;
        case Step::Apply:
            ok = quote.apply(feed.quote);
            break;
        case Step::Gap:
            quote.clear();
            resubscribe(feed, quote.sequence);
            return;
        case Step::Stale:
        case Step::Cold:
            return;
        }
        if (!ok) {
            ++off_grid_;
            return;
        }
        if (!states_top(feed.quote)) {
            return; // a mark or index move, nothing the quote carries
        }
        if (const auto bbo = quote.bbo(ts_event, received); bbo.has_value()) {
            state_->sink->on_bbo(*bbo);
            quoted_.insert(base.id.packed);
        }
    }

    void on_book(const WireFeed& feed,
                 const InstrumentBase& base,
                 UnixNanos ts_event,
                 UnixNanos received) noexcept {
        const auto found = books_.find(base.id.packed);
        if (found == books_.end()) {
            return;
        }
        auto& book = found->second;
        bool ok = false;
        switch (book.sequence.offer(feed.sequence, feed.prev_sequence)) {
        case Step::Snapshot:
            ok = book.replace(feed.book);
            break;
        case Step::Apply:
            ok = book.apply(feed.book);
            break;
        case Step::Gap:
            book.clear();
            resubscribe(feed, book.sequence);
            return;
        case Step::Stale:
        case Step::Cold:
            return;
        }
        if (!ok) {
            ++off_grid_;
            return;
        }
        // Asked before the snapshot is built, which is the expensive half.
        if (!state_->sink->wants_depth(base.id, received)) {
            return;
        }
        if (const auto snapshot = book.snapshot(book.sequence.last(), ts_event, received);
            snapshot.has_value()) {
            state_->sink->on_depth(*snapshot);
        }
    }

    // On the socket the gap arrived on, which is the one that carries the
    // selector: the venue answers with a fresh ack and snapshot there.
    void resubscribe(const WireFeed& feed, Sequence& sequence) noexcept {
        if (!sequence.take_resubscribe()) {
            return;
        }
        const std::array<std::string_view, 1> one{feed.selector};
        if (pool_.reply(frame(feed.stream, one))) {
            ++resubscribes_;
        }
    }

    [[nodiscard]] std::string frame(std::string_view stream,
                                    std::span<const std::string_view> selectors) {
        std::string out{R"({"jsonrpc":"2.0","method":"subscribe","params":{"stream":")"};
        out.append(stream);
        out.append(R"(","selectors":[)");
        for (std::size_t i = 0; i < selectors.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out.push_back('"');
            out.append(selectors[i]);
            out.push_back('"');
        }
        out.append(R"(]},"id":)");
        out.append(std::to_string(next_id_++));
        out.push_back('}');
        return out;
    }

    // Called on every open of ONE socket with that socket's keys.
    [[nodiscard]] std::vector<std::string> build_frames(std::span<const std::string> keys) {
        std::vector<std::string> frames;
        std::vector<std::string_view> selectors;
        for (const auto stream : k_stream_order) {
            selectors.clear();
            for (const std::string_view key : keys) {
                const auto cut = key.find(k_key_sep);
                if (cut != std::string_view::npos && key.substr(0, cut) == stream) {
                    selectors.push_back(key.substr(cut + 1));
                }
            }
            for (std::size_t at = 0; at < selectors.size(); at += ep::k_max_selectors_per_frame) {
                const auto take = std::min(ep::k_max_selectors_per_frame, selectors.size() - at);
                frames.push_back(frame(stream, std::span{selectors}.subspan(at, take)));
            }
        }
        return frames;
    }

    // THE ONE LINE THAT SAYS HOW MUCH OF THE CATALOGUE WAS PRICED. Frame
    // totals cannot: a handful of busy contracts keep them high while the rest
    // of the catalogue sits blank.
    void report() const noexcept {
        if (!has_logger()) {
            return;
        }
        const auto live_books = static_cast<std::size_t>(std::ranges::count_if(
            books_, [](const auto& entry) { return entry.second.sequence.live(); }));
        std::uint64_t gaps = 0;
        for (const auto& [packed, quote] : quotes_) {
            gaps += quote.sequence.gaps();
        }
        for (const auto& [packed, book] : books_) {
            gaps += book.sequence.gaps();
        }
        RLOG_INFO("grvt: {} of {} instruments quoted, {} of {} depth books live — {} chain "
                  "gap(s), {} resubscribe(s), {} refusal(s) (last code {}), {} off-grid, {} "
                  "unreadable",
                  quoted_.size(),
                  quotes_.size(),
                  live_books,
                  books_.size(),
                  gaps,
                  resubscribes_,
                  refusals_,
                  last_error_code_,
                  off_grid_,
                  unreadable_);
    }

    std::shared_ptr<VenueState> state_;
    PublicHandler handler_;
    net::WsPool pool_;
    std::unordered_set<std::string> desired_;
    std::unordered_map<std::uint64_t, Quote> quotes_;
    std::unordered_map<std::uint64_t, DepthBook> books_;
    std::unordered_set<std::uint64_t> quoted_;
    std::uint64_t next_id_{1};
    std::uint64_t refusals_{0};
    std::int64_t last_error_code_{0};
    std::uint64_t resubscribes_{0};
    std::uint64_t off_grid_{0};
    std::uint64_t unreadable_{0};
    bool started_{false};
};

void PublicHandler::on_message(std::string_view payload, UnixNanos received) noexcept {
    owner_->on_frame(payload, received);
}

} // namespace

std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state) {
    return std::make_unique<GrvtData>(std::move(state));
}

} // namespace raikiri::venues::grvt::detail
