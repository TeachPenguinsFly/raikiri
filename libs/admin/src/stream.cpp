#include <raikiri/admin/format.hpp>
#include <raikiri/admin/stream.hpp>

#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <charconv>
#include <deque>
#include <glaze/glaze.hpp>
#include <memory>
#include <utility>

namespace raikiri::admin {

// named namespace: glaze reflection needs linkage
namespace stream_wire {

// the contract's OrderEvent and Fill; the exec ring is their only live source
struct OrderEventRow {
    std::string client_order_id;
    std::string instrument_id;
    std::string event_type;
    std::string price;
    std::string qty;
    std::string leaves;
    std::string fee;
    std::string exec_id;
    std::string ts_event;
};

struct FillRow {
    std::string venue;
    std::string exec_id;
    std::string client_order_id;
    std::string exchange_order_id;
    std::string instrument_id;
    std::string side;
    std::string price;
    std::string qty;
    std::string fee;
    std::string liquidity;
    std::string ts_event;
};

// ids stay text until read as digits
struct Message {
    std::string op;
    std::string id;
    std::vector<std::string> quotes;
    std::string book;
    std::vector<std::string> greeks;
    std::vector<std::string> funding;
    std::vector<std::string> ticker;
    std::vector<std::string> events;
    // raw so the cadence never passes through a double
    std::optional<glz::raw_json> hz;
    std::optional<glz::raw_json> levels;
};

} // namespace stream_wire

namespace {

// glaze omits an empty optional; the contract's absent fields rely on it
constexpr glz::opts k_write_opts{.skip_null_members = true};

// unknown keys are ignored for forward compatibility; a subscribe naming no channel is refused
constexpr glz::opts k_read_opts{.error_on_unknown_keys = false};

constexpr std::size_t k_max_id_bytes = 64;
constexpr std::size_t k_max_instruments = 512;

template <typename T> void append_json(std::string& out, const T& value) {
    std::string text;
    if (const auto ec = glz::write<k_write_opts>(value, text); ec) {
        text = "null";
    }
    out += text;
}

std::string frame_head(std::string_view type, std::string_view id) {
    std::string out{R"({"type":")"};
    out += type;
    out += R"(","id":)";
    append_json(out, id);
    return out;
}

void append_ts(std::string& out, UnixNanos ts) {
    out += R"(,"ts":")";
    out += digits(ts);
    out += '"';
}

// the frame's `type` matches its payload field in every state variant
template <typename T>
std::string list_frame(std::string_view type, std::string_view id, UnixNanos ts, std::span<const T> rows) {
    auto out = frame_head(type, id);
    append_ts(out, ts);
    out += ",\"";
    out += type;
    out += "\":[";
    bool first = true;
    for (const auto& row : rows) {
        if (!std::exchange(first, false)) {
            out += ',';
        }
        append_json(out, row);
    }
    out += "]}";
    return out;
}

template <typename T> std::string object_frame(std::string_view type, std::string_view id, UnixNanos ts, const T& row) {
    auto out = frame_head(type, id);
    append_ts(out, ts);
    out += ",\"";
    out += type;
    out += "\":";
    append_json(out, row);
    out += '}';
    return out;
}

std::optional<InstrumentId> read_id(std::string_view text) noexcept {
    std::uint64_t packed = 0;
    const auto [stop, ec] = std::from_chars(text.data(), text.data() + text.size(), packed);
    if (ec != std::errc{} || stop != text.data() + text.size()) {
        return std::nullopt;
    }
    return InstrumentId{packed};
}

// `positions` and `health` arrive via `events` but are sampled state
std::optional<Feed> read_event(std::string_view name) noexcept {
    if (name == "orders") {
        return Feed::Orders;
    }
    if (name == "fills") {
        return Feed::Fills;
    }
    if (name == "recon") {
        return Feed::Recon;
    }
    if (name == "positions") {
        return Feed::Positions;
    }
    if (name == "health") {
        return Feed::Health;
    }
    return std::nullopt;
}

std::optional<std::int64_t> read_hz(std::string_view text) noexcept {
    const auto value = parse_number_e9(text);
    if (!value.has_value() || value->is_undef()) {
        return std::nullopt;
    }
    return value->raw / 1'000'000; // e9 -> millihertz
}

std::expected<std::vector<InstrumentId>, Problem> read_ids(const std::vector<std::string>& text) {
    if (text.size() > k_max_instruments) {
        return std::unexpected(Problem{.title = "too many instruments in one subscription", .status = 400});
    }
    std::vector<InstrumentId> out;
    out.reserve(text.size());
    for (const auto& one : text) {
        const auto id = read_id(one);
        if (!id.has_value()) {
            return std::unexpected(Problem{.title = "instrument id must be digits", .status = 400, .detail = one});
        }
        out.push_back(*id);
    }
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += ',';
        }
        out += part;
    }
    return out;
}

} // namespace

bool sequenced(Feed feed) noexcept {
    switch (feed) {
    case Feed::Orders:
    case Feed::Fills:
    case Feed::Recon:
        return true;
    case Feed::Quotes:
    case Feed::Book:
    case Feed::Greeks:
    case Feed::Funding:
    case Feed::Ticker:
    case Feed::Positions:
    case Feed::Health:
        return false;
    }
    return false;
}

std::expected<ClientMessage, Problem> parse_client_message(std::string_view text) {
    // copied: glaze needs a null-terminated buffer
    const std::string buffer{text};
    stream_wire::Message wire{};
    if (const auto ec = glz::read<k_read_opts>(wire, buffer); ec) {
        return std::unexpected(Problem{.title = "message is not a stream message", .status = 400});
    }

    ClientMessage out{};
    if (wire.op == "subscribe") {
        out.op = Op::Subscribe;
    } else if (wire.op == "unsubscribe") {
        out.op = Op::Unsubscribe;
    } else {
        return std::unexpected(Problem{.title = "op must be subscribe or unsubscribe", .status = 400});
    }
    if (wire.id.empty() || wire.id.size() > k_max_id_bytes) {
        return std::unexpected(Problem{.title = "subscription id must be 1 to 64 characters", .status = 400});
    }
    out.id = wire.id;
    if (out.op == Op::Unsubscribe) {
        return out; // the handle is the whole message
    }

    auto quotes = read_ids(wire.quotes);
    if (!quotes.has_value()) {
        return std::unexpected(quotes.error());
    }
    out.quotes = std::move(*quotes);
    auto greeks = read_ids(wire.greeks);
    if (!greeks.has_value()) {
        return std::unexpected(greeks.error());
    }
    out.greeks = std::move(*greeks);
    auto funding = read_ids(wire.funding);
    if (!funding.has_value()) {
        return std::unexpected(funding.error());
    }
    out.funding = std::move(*funding);
    auto ticker = read_ids(wire.ticker);
    if (!ticker.has_value()) {
        return std::unexpected(ticker.error());
    }
    out.ticker = std::move(*ticker);
    if (!wire.book.empty()) {
        out.book = read_id(wire.book);
        if (!out.book.has_value()) {
            return std::unexpected(
                Problem{.title = "instrument id must be digits", .status = 400, .detail = wire.book});
        }
    }
    for (const auto& name : wire.events) {
        const auto kind = read_event(name);
        if (!kind.has_value()) {
            return std::unexpected(Problem{.title = "unknown event kind", .status = 400, .detail = name});
        }
        out.events.push_back(*kind);
    }
    if (wire.hz.has_value()) {
        out.millihertz = read_hz(wire.hz->str);
        if (!out.millihertz.has_value()) {
            return std::unexpected(Problem{.title = "hz must be a number", .status = 400, .detail = wire.hz->str});
        }
    }
    if (wire.levels.has_value()) {
        const auto asked = read_id(wire.levels->str);
        if (!asked.has_value() || asked->packed == 0) {
            return std::unexpected(
                Problem{.title = "levels must be a positive whole number", .status = 400, .detail = wire.levels->str});
        }
        out.levels = static_cast<std::size_t>(asked->packed);
    }

    int channels = 0;
    channels += out.quotes.empty() ? 0 : 1;
    channels += out.book.has_value() ? 1 : 0;
    channels += out.greeks.empty() ? 0 : 1;
    channels += out.funding.empty() ? 0 : 1;
    channels += out.ticker.empty() ? 0 : 1;
    channels += out.events.empty() ? 0 : 1;
    if (channels == 0) {
        return std::unexpected(Problem{.title = "subscribe named no channel",
                                       .status = 400,
                                       .detail = "one of quotes, book, greeks, funding, ticker, events"});
    }
    if (channels > 1) {
        // one cadence per ack, so one channel per handle
        return std::unexpected(
            Problem{.title = "one channel per subscription", .status = 400, .detail = "the ack states one cadence"});
    }
    return out;
}

std::int64_t default_millihertz(Feed feed, std::size_t instruments) noexcept {
    switch (feed) {
    case Feed::Quotes:
        // a focused view gets the sampling rate; a watchlist gets 10 Hz
        return instruments <= 1 ? 30'000 : 10'000;
    case Feed::Book:
        // one ladder gets 5 Hz, several get 1 Hz; the top of book is on the quotes feed
        return instruments <= 1 ? 5'000 : 1'000;
    case Feed::Greeks:
    case Feed::Funding:
    case Feed::Ticker:
    case Feed::Positions:
    case Feed::Health:
        return 1'000;
    case Feed::Orders:
    case Feed::Fills:
    case Feed::Recon:
        return 0; // sequenced: drained as fast as the socket takes them
    }
    return 1'000;
}

std::int64_t negotiate_millihertz(Feed feed, std::size_t instruments, std::optional<std::int64_t> asked) noexcept {
    if (sequenced(feed)) {
        return 0;
    }
    if (!asked.has_value()) {
        return default_millihertz(feed, instruments);
    }
    return std::clamp(*asked, k_min_millihertz, k_max_millihertz);
}

std::uint64_t period_ns(std::int64_t millihertz) noexcept {
    if (millihertz <= 0) {
        return 0;
    }
    return 1'000'000'000'000ULL / static_cast<std::uint64_t>(millihertz);
}

bool ChangeGate::moved(std::size_t slot, std::uint32_t version) const noexcept {
    return slot < versions_.size() && version != 0 && versions_[slot] != version;
}

void ChangeGate::accept(std::size_t slot, std::uint32_t version) noexcept {
    if (slot < versions_.size()) {
        versions_[slot] = version;
    }
}

std::optional<Quote> quote_of(const Bbo& bbo, const InstrumentBase& base) {
    if (bbo.bid.is_undef() || bbo.ask.is_undef() || bbo.bid_qty.is_undef() || bbo.ask_qty.is_undef()) {
        return std::nullopt;
    }
    return Quote{.instrument_id = digits(bbo.instrument),
                 .bid = price(bbo.bid, base),
                 .bid_size = size(bbo.bid_qty, base),
                 .ask = price(bbo.ask, base),
                 .ask_size = size(bbo.ask_qty, base),
                 .ts_event = digits(bbo.ts_event)};
}

Book book_of(const BookSnapshot& snapshot, const InstrumentBase& base, std::size_t levels) {
    Book out{};
    out.instrument_id = digits(snapshot.instrument);
    out.ts_event = digits(snapshot.ts_event);
    // levels past the count are stale; the writer only overwrites what it filled
    const auto cap = std::min<std::size_t>(levels, k_book_levels);
    const auto bids = std::min<std::size_t>(snapshot.bid_count, cap);
    const auto asks = std::min<std::size_t>(snapshot.ask_count, cap);
    out.bids.reserve(bids);
    out.asks.reserve(asks);
    for (std::size_t i = 0; i < bids; ++i) {
        out.bids.push_back({.price = price(snapshot.bids[i].price, base), .size = size(snapshot.bids[i].qty, base)});
    }
    for (std::size_t i = 0; i < asks; ++i) {
        out.asks.push_back({.price = price(snapshot.asks[i].price, base), .size = size(snapshot.asks[i].qty, base)});
    }
    return out;
}

Greeks greeks_of(const OptionGreeks& greeks) {
    return Greeks{.instrument_id = digits(greeks.instrument),
                  .delta = decimal(greeks.delta),
                  .gamma = decimal(greeks.gamma),
                  .vega = decimal(greeks.vega),
                  .theta = decimal(greeks.theta),
                  .rho = decimal(greeks.rho),
                  .mark_iv = decimal(greeks.mark_iv),
                  .bid_iv = decimal(greeks.bid_iv),
                  .ask_iv = decimal(greeks.ask_iv),
                  .underlying_price = decimal(greeks.underlying_price),
                  .ts_event = digits(greeks.ts_event)};
}

Funding funding_of(const FundingUpdate& funding, const InstrumentBase& base) {
    Funding out{};
    out.instrument_id = digits(funding.instrument);
    out.rate = decimal(funding.rate);
    out.predicted_rate = decimal(funding.predicted_rate);
    out.mark_price = price(funding.mark_price, base);
    out.next_funding_time = digits(funding.next_funding_time);
    // null when the venue did not say; zero would be read as an interval
    if (funding.interval_s != 0) {
        out.interval_seconds = static_cast<std::int64_t>(funding.interval_s);
    }
    if (funding.accrual == FundingAccrual::Discrete) {
        out.accrual = "discrete";
    } else if (funding.accrual == FundingAccrual::Continuous) {
        out.accrual = "continuous";
    }
    out.ts_event = digits(funding.ts_event);
    return out;
}

Position position_of(const shm::PositionCell& cell, const InstrumentBase& base, UnixNanos now) {
    Position out{};
    out.instrument_id = digits(cell.report.instrument);
    // positions are per account; the segment has no strategy dimension
    out.strategy_id = 0;
    // the gateway holds only the venue's side, so `qty` equals `venue_qty` here
    out.qty = size(cell.report.net_qty, base);
    // avg_entry_price stays empty; the venue's average is not this system's cost basis
    out.ts_updated = digits(cell.ts_pulled);
    const bool fresh = cell.ts_pulled.ns != 0 && now.ns < cell.ts_pulled.ns + k_position_stale_ns;
    if (fresh) {
        out.venue_qty = out.qty;
        out.venue_reported_at = digits(cell.report.ts_venue);
    }
    return out;
}

std::string ack_frame(std::string_view id, std::optional<std::int64_t> millihertz) {
    auto out = frame_head("ack", id);
    if (millihertz.has_value()) {
        // rendered through the shared formatter so 500 millihertz prints 0.5
        out += R"(,"hz":)";
        out += decimal(FixedE9::from_raw(*millihertz * 1'000'000));
    }
    out += '}';
    return out;
}

std::string quotes_frame(std::string_view id, UnixNanos ts, std::span<const Quote> quotes) {
    return list_frame("quotes", id, ts, quotes);
}

std::string book_frame(std::string_view id, UnixNanos ts, const Book& book) {
    return object_frame("book", id, ts, book);
}

std::string greeks_frame(std::string_view id, UnixNanos ts, std::span<const Greeks> greeks) {
    return list_frame("greeks", id, ts, greeks);
}

Ticker ticker_of(const TickerStats& stats) {
    Ticker out{};
    out.instrument_id = digits(stats.instrument);
    // not scaled by tick or lot
    out.volume_24h_base = decimal(stats.volume_24h_base);
    out.volume_24h_quote = decimal(stats.volume_24h_quote);
    out.ts_event = digits(stats.ts_event);
    return out;
}

std::string ticker_frame(std::string_view id, UnixNanos ts, std::span<const Ticker> rows) {
    return list_frame("ticker", id, ts, rows);
}

std::string funding_frame(std::string_view id, UnixNanos ts, std::span<const Funding> funding) {
    return list_frame("funding", id, ts, funding);
}

std::string positions_frame(std::string_view id, UnixNanos ts, std::span<const Position> positions) {
    return list_frame("positions", id, ts, positions);
}

std::string health_frame(std::string_view id, UnixNanos ts, const Health& health) {
    return object_frame("health", id, ts, health);
}

std::string order_event_frame(std::string_view id,
                              std::uint64_t seq,
                              const raikiri::OrderEvent& event,
                              const InstrumentBase& base) {
    const stream_wire::OrderEventRow row{.client_order_id = digits(event.client_order_id.raw),
                                         .instrument_id = digits(event.instrument),
                                         .event_type = std::string{to_string(event.type)},
                                         .price = price(event.price, base),
                                         .qty = size(event.qty, base),
                                         .leaves = size(event.leaves_qty, base),
                                         .fee = decimal(event.fee),
                                         .exec_id = std::string{event.exec_id.view()},
                                         .ts_event = digits(event.ts_event)};
    auto out = frame_head("event", id);
    out += R"(,"seq":")";
    out += digits(seq);
    out += R"(","order_event":)";
    append_json(out, row);
    out += '}';
    return out;
}

std::string
fill_frame(std::string_view id, std::uint64_t seq, const raikiri::OrderEvent& event, const InstrumentBase& base) {
    const stream_wire::FillRow row{.venue = std::string{to_string(event.instrument.venue())},
                                   .exec_id = std::string{event.exec_id.view()},
                                   .client_order_id = digits(event.client_order_id.raw),
                                   .exchange_order_id = std::string{event.exchange_order_id.view()},
                                   .instrument_id = digits(event.instrument),
                                   .side = std::string{to_string(event.side)},
                                   .price = price(event.price, base),
                                   .qty = size(event.qty, base),
                                   .fee = decimal(event.fee),
                                   .liquidity = std::string{to_string(event.liquidity)},
                                   .ts_event = digits(event.ts_event)};
    auto out = frame_head("event", id);
    out += R"(,"seq":")";
    out += digits(seq);
    out += R"(","fill":)";
    append_json(out, row);
    out += '}';
    return out;
}

std::string gap_frame(std::string_view id, std::uint64_t missed) {
    auto out = frame_head("gap", id);
    out += R"(,"missed":")";
    out += digits(missed);
    out += R"("})";
    return out;
}

std::string problem_frame(std::string_view id, const Problem& problem) {
    std::string out{R"({"type":"problem")"};
    if (!id.empty()) {
        out += R"(,"id":)";
        append_json(out, id);
    }
    out += R"(,"problem":)";
    append_json(out, problem);
    out += '}';
    return out;
}

std::vector<Target>
resolve_targets(const Registry& registry, std::span<const InstrumentId> ids, std::vector<std::string>& unknown) {
    std::vector<Target> out;
    out.reserve(ids.size());
    for (const auto id : ids) {
        const auto row = registry.catalogue_row(id);
        const std::size_t cell = id.symbol().raw;
        // an id past the tables gets the same answer as an unlisted one
        if (!row.has_value() || cell >= shm::k_bbo_cells) {
            unknown.push_back(digits(id));
            continue;
        }
        out.push_back({.gateway = row->gateway, .listed = row->instrument, .id = id, .cell = cell});
    }
    return out;
}

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websock = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::string_view k_path = "/v1/stream";

// the largest honest message is a watchlist of a few hundred ids
constexpr std::uint64_t k_max_message_bytes = 64 * 1024;
constexpr std::size_t k_max_feeds = 64;

// control frames queue here; state frames are dropped instead
constexpr std::size_t k_max_queued = 64;

// bounded so one busy venue cannot hold the pass open
constexpr std::size_t k_max_polls_per_pass = 256;

struct EventSource {
    const Gateway* gateway{nullptr};
    // a sequence from the old ring means nothing after a re-attach
    const shm::ExecRing* ring{nullptr};
    shm::ExecReader reader;
};

// the exec ring is not signalled, so it is polled at this pace
constexpr std::uint64_t k_event_poll_ns = 10'000'000;

struct FeedState {
    std::string id;
    Feed kind{Feed::Quotes};
    std::int64_t millihertz{0};
    std::uint64_t period{0};
    MonoNanos due{};
    // the requested ids, kept so targets can be rebuilt after a re-attach
    std::vector<InstrumentId> asked;
    std::vector<Target> targets;
    ChangeGate gate;
    std::vector<EventSource> sources;
    std::uint64_t seq{0};
    // book subscriptions only; per subscription
    std::size_t levels{k_default_levels};
};

// `version` is the copy's, not the probe's; the writer may move between the reads
template <typename Record> struct Changed {
    std::size_t slot{0};
    std::uint32_t version{0};
    const InstrumentBase* base{nullptr};
    Record record{};
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const Registry& registry, const Clock& clock)
        : ws_{std::move(socket)}, registry_{&registry}, clock_{&clock}, epoch_{registry.epoch()} {}

    void run() {
        // read the upgrade request first; the path is part of the contract
        request_ = std::make_unique<http::request_parser<http::empty_body>>();
        auto self = shared_from_this();
        http::async_read(
            ws_.next_layer(), buffer_, *request_, [self](beast::error_code ec, std::size_t) { self->on_request(ec); });
    }

    // returns frames sent, so an idle caller may sleep
    std::size_t tick(MonoNanos mono, UnixNanos wall) {
        if (closed_ || !open_) {
            return 0;
        }
        resync();
        std::size_t sent = drain_events();
        // round robin; `due` advances only for a feed actually sampled, or later feeds starve
        for (std::size_t step = 0; step < feeds_.size(); ++step) {
            if (busy()) {
                break; // the socket is behind; every remaining feed stays due
            }
            auto& feed = feeds_[(next_feed_ + step) % feeds_.size()];
            if (sequenced(feed.kind) || mono.ns < feed.due.ns) {
                continue;
            }
            // scheduled from now so a stalled client gets no burst
            feed.due = mono + feed.period;
            sent += sample(feed, mono, wall) ? 1U : 0U;
        }
        if (!feeds_.empty()) {
            next_feed_ = (next_feed_ + 1) % feeds_.size();
        }
        return sent;
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }

    [[nodiscard]] std::uint64_t next_due_ns(std::uint64_t mono) const noexcept {
        std::uint64_t due = UINT64_MAX;
        if (closed_ || !open_) {
            return due;
        }
        for (const auto& feed : feeds_) {
            due = std::min(due, sequenced(feed.kind) ? mono + k_event_poll_ns : feed.due.ns);
        }
        return due;
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        beast::error_code ec;
        (void)beast::get_lowest_layer(ws_).socket().shutdown(tcp::socket::shutdown_both, ec);
    }

private:
    void on_request(beast::error_code ec) {
        if (ec) {
            close();
            return;
        }
        const auto& request = request_->get();
        const auto target = std::string_view{request.target()};
        if (!websock::is_upgrade(request) || target.substr(0, target.find('?')) != k_path) {
            close();
            return;
        }
        ws_.read_message_max(k_max_message_bytes);
        ws_.text(true);
        auto self = shared_from_this();
        ws_.async_accept(request, [self](beast::error_code accept_ec) {
            if (accept_ec) {
                self->close();
                return;
            }
            self->open_ = true;
            self->read();
        });
    }

    void read() {
        auto self = shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                self->close();
                return;
            }
            const auto data = self->buffer_.data();
            self->handle(std::string_view{static_cast<const char*>(data.data()), data.size()});
            self->buffer_.consume(self->buffer_.size());
            if (!self->closed_) {
                self->read();
            }
        });
    }

    void handle(std::string_view text) {
        const auto message = parse_client_message(text);
        if (!message.has_value()) {
            enqueue(problem_frame({}, message.error()));
            return;
        }
        switch (message->op) {
        case Op::Subscribe:
            subscribe(*message);
            break;
        case Op::Unsubscribe:
            unsubscribe(*message);
            break;
        }
    }

    [[nodiscard]] std::vector<const Gateway*> attached() const {
        std::vector<const Gateway*> out;
        out.reserve(registry_->size());
        for (std::size_t i = 0; i < registry_->size(); ++i) {
            out.push_back(registry_->at(i));
        }
        return out;
    }

    // every catalogued instrument with a position cell; positions follow the account
    [[nodiscard]] static std::vector<Target> position_targets(const std::vector<const Gateway*>& gateways) {
        std::vector<Target> out;
        for (const auto* gateway : gateways) {
            for (const auto& listed : gateway->channels.catalogue->all()) {
                const auto id = base_of(listed).id;
                if (const std::size_t cell = id.symbol().raw; cell < shm::k_position_cells) {
                    out.push_back({.gateway = gateway, .listed = &listed, .id = id, .cell = cell});
                }
            }
        }
        return out;
    }

    [[nodiscard]] static std::vector<EventSource> exec_sources(const std::vector<const Gateway*>& gateways) {
        std::vector<EventSource> out;
        out.reserve(gateways.size());
        for (const auto* gateway : gateways) {
            out.push_back({.gateway = gateway,
                           .ring = gateway->channels.exec_events,
                           .reader = shm::ExecReader{gateway->channels.exec_events}});
        }
        return out;
    }

    // re-resolve targets when the registry epoch moves after a gateway re-attach
    void resync() {
        const auto epoch = registry_->epoch();
        if (epoch == epoch_) {
            return;
        }
        epoch_ = epoch;
        const auto gateways = attached();
        for (auto& feed : feeds_) {
            if (sequenced(feed.kind)) {
                // reset only the restarted gateway's ring cursor; other venues' queued events must not be skipped
                for (auto& source : feed.sources) {
                    if (source.ring != source.gateway->channels.exec_events) {
                        source.ring = source.gateway->channels.exec_events;
                        source.reader = shm::ExecReader{source.ring};
                    }
                }
                continue;
            }
            if (feed.kind == Feed::Health) {
                continue; // reads the registry per frame and holds nothing
            }
            if (feed.kind == Feed::Positions) {
                feed.targets = position_targets(gateways);
            } else {
                std::vector<std::string> unknown;
                feed.targets = resolve_targets(*registry_, feed.asked, unknown);
                if (!unknown.empty()) {
                    // ids the restarted gateway no longer lists get a problem frame
                    enqueue(problem_frame(feed.id,
                                          {.title = "the gateway restarted and no longer lists "
                                                    "these instruments",
                                           .status = 404,
                                           .detail = join(unknown)}));
                }
            }
            // empty gate so the first tick after a re-attach is a snapshot
            feed.gate = ChangeGate{feed.targets.size()};
        }
    }

    void subscribe(const ClientMessage& message) {
        if (feeds_.size() >= k_max_feeds) {
            enqueue(problem_frame(message.id, {.title = "too many subscriptions", .status = 429}));
            return;
        }
        // re-subscribing a handle replaces it
        drop(message.id);

        if (!message.quotes.empty()) {
            open_state(message, Feed::Quotes, message.quotes);
            return;
        }
        if (message.book.has_value()) {
            open_state(message, Feed::Book, std::span<const InstrumentId>{&*message.book, 1U});
            return;
        }
        if (!message.greeks.empty()) {
            open_state(message, Feed::Greeks, message.greeks);
            return;
        }
        if (!message.funding.empty()) {
            open_state(message, Feed::Funding, message.funding);
            return;
        }
        if (!message.ticker.empty()) {
            open_state(message, Feed::Ticker, message.ticker);
            return;
        }
        open_events(message);
    }

    void open_state(const ClientMessage& message, Feed kind, std::span<const InstrumentId> ids) {
        FeedState feed{};
        feed.id = message.id;
        feed.kind = kind;
        // clamped to the snapshot depth, not refused
        feed.levels = std::min<std::size_t>(message.levels.value_or(k_default_levels), k_book_levels);
        feed.asked.assign(ids.begin(), ids.end());
        std::vector<std::string> unknown;
        feed.targets = resolve_targets(*registry_, ids, unknown);
        if (!unknown.empty()) {
            // unresolved ids get a problem frame
            enqueue(problem_frame(
                message.id, {.title = "no gateway holds these instruments", .status = 404, .detail = join(unknown)}));
        }
        if (feed.targets.empty()) {
            return;
        }
        // empty gate and due now: the first tick is a snapshot
        feed.gate = ChangeGate{feed.targets.size()};
        feed.millihertz = negotiate_millihertz(kind, feed.targets.size(), message.millihertz);
        feed.period = period_ns(feed.millihertz);
        feed.due = clock_->mono();
        enqueue(ack_frame(message.id, feed.millihertz));
        feeds_.push_back(std::move(feed));
    }

    void open_events(const ClientMessage& message) {
        const auto gateways = attached();
        std::optional<std::int64_t> acked;
        bool created = false;
        for (const auto kind : message.events) {
            switch (kind) {
            case Feed::Recon:
                // no gateway channel carries recon verdicts, so refuse
                enqueue(problem_frame(message.id,
                                      {.title = "no live channel for reconciliation breaks",
                                       .status = 501,
                                       .detail = "reconciliation is written by strategyd; read /v1/recon-breaks"}));
                continue;
            case Feed::Orders:
            case Feed::Fills: {
                FeedState feed{};
                feed.id = message.id;
                feed.kind = kind;
                feed.sources = exec_sources(gateways);
                feeds_.push_back(std::move(feed));
                created = true;
                continue;
            }
            case Feed::Positions: {
                FeedState feed{};
                feed.id = message.id;
                feed.kind = kind;
                feed.targets = position_targets(gateways);
                feed.gate = ChangeGate{feed.targets.size()};
                feed.millihertz = negotiate_millihertz(kind, feed.targets.size(), message.millihertz);
                feed.period = period_ns(feed.millihertz);
                feed.due = clock_->mono();
                acked = feed.millihertz;
                feeds_.push_back(std::move(feed));
                created = true;
                continue;
            }
            case Feed::Health: {
                FeedState feed{};
                feed.id = message.id;
                feed.kind = kind;
                feed.millihertz = negotiate_millihertz(kind, 0, message.millihertz);
                feed.period = period_ns(feed.millihertz);
                feed.due = clock_->mono();
                acked = feed.millihertz;
                feeds_.push_back(std::move(feed));
                created = true;
                continue;
            }
            case Feed::Quotes:
            case Feed::Book:
            case Feed::Greeks:
            case Feed::Funding:
            case Feed::Ticker:
                continue; // not reachable: the events array names none of these
            }
        }
        if (created) {
            enqueue(ack_frame(message.id, acked));
        }
    }

    void unsubscribe(const ClientMessage& message) {
        if (drop(message.id) == 0) {
            enqueue(problem_frame(message.id, {.title = "no such subscription", .status = 404}));
            return;
        }
        enqueue(ack_frame(message.id, std::nullopt));
    }

    std::size_t drop(std::string_view id) {
        return std::erase_if(feeds_, [id](const FeedState& feed) { return feed.id == id; });
    }

    bool sample(FeedState& feed, MonoNanos mono, UnixNanos wall) {
        switch (feed.kind) {
        case Feed::Quotes:
            return sample_quotes(feed, wall);
        case Feed::Book:
            return sample_book(feed, wall);
        case Feed::Greeks:
            return sample_greeks(feed, wall);
        case Feed::Funding:
            return sample_funding(feed, wall);
        case Feed::Ticker:
            return sample_ticker(feed, wall);
        case Feed::Positions:
            return sample_positions(feed, wall);
        case Feed::Health:
            // gateways only; `database` is a default here
            enqueue(health_frame(feed.id, wall, registry_->health(mono, wall)));
            return true;
        case Feed::Orders:
        case Feed::Fills:
        case Feed::Recon:
            return false; // sequenced feeds are drained, never sampled
        }
        return false;
    }

    // probe the version, copy only if moved, return the copy's version
    template <typename Record, typename Table>
    [[nodiscard]] std::vector<Changed<Record>> changed(const FeedState& feed,
                                                       Table* shm::GatewayChannels::*channel) const {
        std::vector<Changed<Record>> out;
        for (std::size_t slot = 0; slot < feed.targets.size(); ++slot) {
            const auto& target = feed.targets[slot];
            const auto& table = *(target.gateway->channels.*channel);
            if (!feed.gate.moved(slot, table.cells[target.cell].version_now())) {
                continue;
            }
            Changed<Record> one{.slot = slot, .base = &base_of(*target.listed)};
            one.version = table.read(target.cell, one.record);
            if (one.version == 0) {
                continue;
            }
            out.push_back(std::move(one));
        }
        return out;
    }

    // recorded only once the frame reached the socket
    static void remember(FeedState& feed, const std::vector<std::pair<std::size_t, std::uint32_t>>& taken) {
        for (const auto& [slot, version] : taken) {
            feed.gate.accept(slot, version);
        }
    }

    bool sample_quotes(FeedState& feed, UnixNanos wall) {
        std::vector<Quote> rows;
        std::vector<std::pair<std::size_t, std::uint32_t>> taken;
        for (auto& cell : changed<Bbo>(feed, &shm::GatewayChannels::bbo)) {
            auto quote = quote_of(cell.record, *cell.base);
            if (!quote.has_value()) {
                continue; // one-sided: nothing was sent, so nothing is recorded
            }
            rows.push_back(std::move(*quote));
            taken.emplace_back(cell.slot, cell.version);
        }
        if (rows.empty()) {
            return false;
        }
        enqueue(quotes_frame(feed.id, wall, rows));
        remember(feed, taken);
        return true;
    }

    bool sample_book(FeedState& feed, UnixNanos wall) {
        const auto moved = changed<BookSnapshot>(feed, &shm::GatewayChannels::depth);
        if (moved.empty()) {
            return false;
        }
        const auto& cell = moved.front();
        enqueue(book_frame(feed.id, wall, book_of(cell.record, *cell.base, feed.levels)));
        feed.gate.accept(cell.slot, cell.version);
        return true;
    }

    bool sample_greeks(FeedState& feed, UnixNanos wall) {
        const auto moved = changed<OptionGreeks>(feed, &shm::GatewayChannels::greeks);
        if (moved.empty()) {
            return false;
        }
        std::vector<Greeks> rows;
        rows.reserve(moved.size());
        std::vector<std::pair<std::size_t, std::uint32_t>> taken;
        for (const auto& cell : moved) {
            rows.push_back(greeks_of(cell.record));
            taken.emplace_back(cell.slot, cell.version);
        }
        enqueue(greeks_frame(feed.id, wall, rows));
        remember(feed, taken);
        return true;
    }

    bool sample_funding(FeedState& feed, UnixNanos wall) {
        const auto moved = changed<FundingUpdate>(feed, &shm::GatewayChannels::funding);
        if (moved.empty()) {
            return false;
        }
        std::vector<Funding> rows;
        rows.reserve(moved.size());
        std::vector<std::pair<std::size_t, std::uint32_t>> taken;
        for (const auto& cell : moved) {
            rows.push_back(funding_of(cell.record, *cell.base));
            taken.emplace_back(cell.slot, cell.version);
        }
        enqueue(funding_frame(feed.id, wall, rows));
        remember(feed, taken);
        return true;
    }

    bool sample_ticker(FeedState& feed, UnixNanos wall) {
        const auto moved = changed<TickerStats>(feed, &shm::GatewayChannels::ticker_stats);
        if (moved.empty()) {
            return false;
        }
        std::vector<Ticker> rows;
        rows.reserve(moved.size());
        std::vector<std::pair<std::size_t, std::uint32_t>> taken;
        for (const auto& cell : moved) {
            rows.push_back(ticker_of(cell.record));
            taken.emplace_back(cell.slot, cell.version);
        }
        enqueue(ticker_frame(feed.id, wall, rows));
        remember(feed, taken);
        return true;
    }

    bool sample_positions(FeedState& feed, UnixNanos wall) {
        const auto moved = changed<shm::PositionCell>(feed, &shm::GatewayChannels::positions);
        if (moved.empty()) {
            return false;
        }
        std::vector<Position> rows;
        rows.reserve(moved.size());
        std::vector<std::pair<std::size_t, std::uint32_t>> taken;
        for (const auto& cell : moved) {
            rows.push_back(position_of(cell.record, *cell.base, wall));
            taken.emplace_back(cell.slot, cell.version);
        }
        enqueue(positions_frame(feed.id, wall, rows));
        remember(feed, taken);
        return true;
    }

    std::size_t drain_events() {
        // also reached from a write completion; resync before touching targets
        resync();
        std::size_t sent = 0;
        for (auto& feed : feeds_) {
            if (sequenced(feed.kind)) {
                sent += drain(feed);
            }
        }
        return sent;
    }

    std::size_t drain(FeedState& feed) {
        std::size_t sent = 0;
        for (std::size_t pass = 0; pass < k_max_polls_per_pass && !busy(); ++pass) {
            bool any = false;
            for (auto& source : feed.sources) {
                if (busy()) {
                    break;
                }
                shm::ExecEventRec record{};
                switch (source.reader.poll(record)) {
                case shm::PollResult::Empty:
                    break;
                case shm::PollResult::Overrun:
                    any = true;
                    // the gap counts ring items, not this subscription's frames
                    enqueue(gap_frame(feed.id, source.reader.gap()));
                    ++sent;
                    break;
                case shm::PollResult::Item: {
                    any = true;
                    if (feed.kind == Feed::Fills && record.event.type != OrderEventType::Filled) {
                        break; // not this subscription's event, and nothing was lost
                    }
                    const auto* listed = source.gateway->channels.catalogue->find(record.event.instrument);
                    if (listed == nullptr) {
                        break; // a price without its instrument is not a price
                    }
                    ++feed.seq;
                    const auto& base = base_of(*listed);
                    enqueue(feed.kind == Feed::Fills ? fill_frame(feed.id, feed.seq, record.event, base)
                                                     : order_event_frame(feed.id, feed.seq, record.event, base));
                    ++sent;
                    break;
                }
                }
            }
            if (!any) {
                break;
            }
        }
        return sent;
    }

    [[nodiscard]] bool busy() const noexcept { return writing_ || !out_.empty(); }

    // where the next tick starts its walk
    std::size_t next_feed_{0};

    void enqueue(std::string frame) {
        if (out_.size() >= k_max_queued) {
            close(); // a client that will not read is not a client to wait for
            return;
        }
        out_.push_back(std::move(frame));
        pump();
    }

    void pump() {
        if (closed_ || !open_ || writing_ || out_.empty()) {
            return;
        }
        writing_buf_ = std::move(out_.front());
        out_.pop_front();
        writing_ = true;
        auto self = shared_from_this();
        ws_.async_write(asio::buffer(writing_buf_), [self](beast::error_code ec, std::size_t) {
            self->writing_ = false;
            if (ec) {
                self->close();
                return;
            }
            // events drain at socket speed, not tick speed
            self->drain_events();
            self->pump();
        });
    }

    websock::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::unique_ptr<http::request_parser<http::empty_body>> request_;
    const Registry* registry_;
    const Clock* clock_;
    // the registry epoch the targets were resolved against
    std::uint64_t epoch_{0};
    std::vector<FeedState> feeds_;
    std::deque<std::string> out_;
    std::string writing_buf_;
    bool writing_{false};
    bool open_{false};
    bool closed_{false};
};

} // namespace

struct Stream::Impl {
    Impl(void* shared, const Registry& reg, const Clock& clk, std::string addr, std::uint16_t p)
        : owned{shared == nullptr ? std::make_unique<asio::io_context>(1) : nullptr},
          ioc{shared == nullptr ? *owned : *static_cast<asio::io_context*>(shared)}, registry{reg}, clock{clk},
          address{std::move(addr)}, port{p}, acceptor{ioc} {}

    void accept() {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket), registry, clock);
                sessions.push_back(session);
                session->run();
            }
            if (running) {
                accept();
            }
        });
    }

    std::unique_ptr<asio::io_context> owned;
    asio::io_context& ioc;
    const Registry& registry;
    const Clock& clock;
    std::string address;
    std::uint16_t port;
    tcp::acceptor acceptor;
    std::vector<std::shared_ptr<Session>> sessions;
    bool running{false};
};

Stream::Stream(const Registry& registry, const Clock& clock, std::string address, std::uint16_t port)
    : impl_{std::make_unique<Impl>(nullptr, registry, clock, std::move(address), port)} {}

Stream::Stream(
    net::IoContext& io, const Registry& registry, const Clock& clock, std::string address, std::uint16_t port)
    : impl_{std::make_unique<Impl>(io.native(), registry, clock, std::move(address), port)} {}

Stream::~Stream() = default;

bool Stream::start() {
    beast::error_code ec;
    const auto addr = asio::ip::make_address(impl_->address, ec);
    if (ec) {
        return false;
    }
    const tcp::endpoint endpoint{addr, impl_->port};
    (void)impl_->acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        return false;
    }
    (void)impl_->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    (void)impl_->acceptor.bind(endpoint, ec);
    if (ec) {
        return false; // the port is taken: a configuration error, not a retry
    }
    (void)impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return false;
    }
    // port 0 lets tests bind any free port
    impl_->port = impl_->acceptor.local_endpoint(ec).port();
    impl_->running = true;
    impl_->accept();
    return true;
}

std::size_t Stream::poll() noexcept {
    std::size_t work = 0;
    try {
        const auto mono = impl_->clock.mono();
        const auto wall = impl_->clock.wall();
        for (const auto& session : impl_->sessions) {
            work += session->tick(mono, wall);
        }
        std::erase_if(impl_->sessions, [](const std::shared_ptr<Session>& session) { return session->closed(); });
        if (impl_->ioc.stopped()) {
            impl_->ioc.restart();
        }
        work += impl_->ioc.poll();
    } catch (...) {
        return work;
    }
    return work;
}

void Stream::stop() noexcept {
    impl_->running = false;
    beast::error_code ec;
    (void)impl_->acceptor.close(ec);
    for (const auto& session : impl_->sessions) {
        session->close();
    }
    impl_->sessions.clear();
    impl_->ioc.stop();
}

MonoNanos Stream::next_due() const noexcept {
    const auto mono = impl_->clock.mono().ns;
    std::uint64_t due = UINT64_MAX;
    for (const auto& session : impl_->sessions) {
        due = std::min(due, session->next_due_ns(mono));
    }
    return MonoNanos{due};
}

std::uint16_t Stream::port() const noexcept {
    return impl_->port;
}

std::size_t Stream::clients() const noexcept {
    return impl_->sessions.size();
}

} // namespace raikiri::admin
