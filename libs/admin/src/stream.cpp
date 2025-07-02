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

// The shapes glaze reflects, named rather than anonymous: reflection reads
// field names off a type with linkage and an anonymous namespace has none.
namespace stream_wire {

// The contract's OrderEvent and Fill. They live here because the exec ring is
// their only live source and nothing outside this file renders one.
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

// The client message as it arrives. Ids are text here and stay text until they
// are read as digits, because that is the only way a 64-bit id survives a
// browser at all.
struct Message {
    std::string op;
    std::string id;
    std::vector<std::string> quotes;
    std::string book;
    std::vector<std::string> greeks;
    std::vector<std::string> funding;
    std::vector<std::string> events;
    // Raw, so the cadence never passes through a double on its way to being an
    // integer count of millihertz.
    std::optional<glz::raw_json> hz;
};

} // namespace stream_wire

namespace {

// glaze omits an empty optional, which is the contract's rule: a field with no
// answer is absent, never null and never zero.
constexpr glz::opts k_write_opts{.skip_null_members = true};

// A key this server does not know is a client one version ahead, not a client
// speaking nonsense. What it asked for is decided by the fields named below,
// and a subscribe that named no channel at all is refused loudly, which is
// what catches a misspelling.
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

// The frame's `type` and its payload field are the same word in every state
// variant, which is why one builder covers all of them.
template <typename T>
std::string
list_frame(std::string_view type, std::string_view id, UnixNanos ts, std::span<const T> rows) {
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

template <typename T>
std::string object_frame(std::string_view type, std::string_view id, UnixNanos ts, const T& row) {
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

// `positions` and `health` arrive through the `events` array because the
// subscribe message has no field of their own — but they are STATE, sampled
// and coalesced like every other table, not sequenced.
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
        return std::unexpected(
            Problem{.title = "too many instruments in one subscription", .status = 400});
    }
    std::vector<InstrumentId> out;
    out.reserve(text.size());
    for (const auto& one : text) {
        const auto id = read_id(one);
        if (!id.has_value()) {
            return std::unexpected(
                Problem{.title = "instrument id must be digits", .status = 400, .detail = one});
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

// ------------------------------------------------------- subscriptions ----

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
    case Feed::Positions:
    case Feed::Health:
        return false;
    }
    return false;
}

std::expected<ClientMessage, Problem> parse_client_message(std::string_view text) {
    // Copied because glaze reads a null-terminated buffer and a WebSocket
    // payload is not one.
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
        return std::unexpected(
            Problem{.title = "op must be subscribe or unsubscribe", .status = 400});
    }
    if (wire.id.empty() || wire.id.size() > k_max_id_bytes) {
        return std::unexpected(
            Problem{.title = "subscription id must be 1 to 64 characters", .status = 400});
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
    if (!wire.book.empty()) {
        out.book = read_id(wire.book);
        if (!out.book.has_value()) {
            return std::unexpected(Problem{
                .title = "instrument id must be digits", .status = 400, .detail = wire.book});
        }
    }
    for (const auto& name : wire.events) {
        const auto kind = read_event(name);
        if (!kind.has_value()) {
            return std::unexpected(
                Problem{.title = "unknown event kind", .status = 400, .detail = name});
        }
        out.events.push_back(*kind);
    }
    if (wire.hz.has_value()) {
        out.millihertz = read_hz(wire.hz->str);
        if (!out.millihertz.has_value()) {
            return std::unexpected(
                Problem{.title = "hz must be a number", .status = 400, .detail = wire.hz->str});
        }
    }

    int channels = 0;
    channels += out.quotes.empty() ? 0 : 1;
    channels += out.book.has_value() ? 1 : 0;
    channels += out.greeks.empty() ? 0 : 1;
    channels += out.funding.empty() ? 0 : 1;
    channels += out.events.empty() ? 0 : 1;
    if (channels == 0) {
        return std::unexpected(Problem{.title = "subscribe named no channel",
                                       .status = 400,
                                       .detail = "one of quotes, book, greeks, funding, events"});
    }
    if (channels > 1) {
        // The ack states ONE cadence, so a handle covers one channel. Nothing
        // stops a client from opening several handles, and then each ack says
        // the truth about the feed it belongs to.
        return std::unexpected(Problem{.title = "one channel per subscription",
                                       .status = 400,
                                       .detail = "the ack states one cadence"});
    }
    return out;
}

// ------------------------------------------------------------ cadence ----

std::int64_t default_millihertz(Feed feed, std::size_t instruments) noexcept {
    switch (feed) {
    case Feed::Quotes:
        // One instrument is a focused view and earns the sampling rate; a
        // watchlist of forty is a display, and a display is 10 Hz.
        return instruments <= 1 ? 30'000 : 10'000;
    case Feed::Book:
        return 30'000;
    case Feed::Greeks:
    case Feed::Funding:
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

std::int64_t negotiate_millihertz(Feed feed,
                                  std::size_t instruments,
                                  std::optional<std::int64_t> asked) noexcept {
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

// --------------------------------------------------- records into rows ----

std::optional<Quote> quote_of(const Bbo& bbo, const InstrumentBase& base) {
    if (bbo.bid.is_undef() || bbo.ask.is_undef() || bbo.bid_qty.is_undef() ||
        bbo.ask_qty.is_undef()) {
        return std::nullopt;
    }
    return Quote{.instrument_id = digits(bbo.instrument),
                 .bid = price(bbo.bid, base),
                 .bid_size = size(bbo.bid_qty, base),
                 .ask = price(bbo.ask, base),
                 .ask_size = size(bbo.ask_qty, base),
                 .ts_event = digits(bbo.ts_event)};
}

Book book_of(const BookSnapshot& snapshot, const InstrumentBase& base) {
    Book out{};
    out.instrument_id = digits(snapshot.instrument);
    out.ts_event = digits(snapshot.ts_event);
    // Levels past the count are the previous snapshot's, still sitting in the
    // cell: the writer overwrites what it filled and nothing else.
    const auto bids = std::min<std::size_t>(snapshot.bid_count, k_book_levels);
    const auto asks = std::min<std::size_t>(snapshot.ask_count, k_book_levels);
    out.bids.reserve(bids);
    out.asks.reserve(asks);
    for (std::size_t i = 0; i < bids; ++i) {
        out.bids.push_back({.price = price(snapshot.bids[i].price, base),
                            .size = size(snapshot.bids[i].qty, base)});
    }
    for (std::size_t i = 0; i < asks; ++i) {
        out.asks.push_back({.price = price(snapshot.asks[i].price, base),
                            .size = size(snapshot.asks[i].qty, base)});
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
    // Null rather than zero when the venue did not say: annualising an hourly
    // rate as an eight-hourly one is wrong by a factor of eight, and a zero
    // interval would be read as a number.
    if (funding.interval_s != 0) {
        out.interval_seconds = static_cast<std::int64_t>(funding.interval_s);
    }
    out.ts_event = digits(funding.ts_event);
    return out;
}

Position position_of(const shm::PositionCell& cell, const InstrumentBase& base, UnixNanos now) {
    Position out{};
    out.instrument_id = digits(cell.report.instrument);
    // The venue reports per account; a strategy is this system's idea and the
    // segment carries no dimension for it.
    out.strategy_id = 0;
    // THE GATEWAY HOLDS ONLY THE VENUE'S HALF, so `qty` and `venue_qty` are
    // the same number here and this feed can never show a break. The book's
    // own quantity lives in strategyd, and a disagreement between the two is
    // what /v1/recon-breaks is for.
    out.qty = size(cell.report.net_qty, base);
    // avg_entry_price stays empty on purpose: a venue recomputes an average by
    // its own rules and this system's cost basis is not that number.
    out.ts_updated = digits(cell.ts_pulled);
    const bool fresh = cell.ts_pulled.ns != 0 && now.ns < cell.ts_pulled.ns + k_position_stale_ns;
    if (fresh) {
        out.venue_qty = out.qty;
        out.venue_reported_at = digits(cell.report.ts_venue);
    }
    return out;
}

// ------------------------------------------------------------- frames ----

std::string ack_frame(std::string_view id, std::optional<std::int64_t> millihertz) {
    auto out = frame_head("ack", id);
    if (millihertz.has_value()) {
        // Small enough to be a JSON number honestly, and rendered through the
        // one formatter so 500 millihertz is 0.5 and not 0.49999999.
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

std::string funding_frame(std::string_view id, UnixNanos ts, std::span<const Funding> funding) {
    return list_frame("funding", id, ts, funding);
}

std::string
positions_frame(std::string_view id, UnixNanos ts, std::span<const Position> positions) {
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

std::string fill_frame(std::string_view id,
                       std::uint64_t seq,
                       const raikiri::OrderEvent& event,
                       const InstrumentBase& base) {
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

// -------------------------------------------------------------- stream ----

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websock = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::string_view k_path = "/v1/stream";

// A confused or hostile client must not make the process allocate: the largest
// honest message is a watchlist of a few hundred ids.
constexpr std::uint64_t k_max_message_bytes = 64 * 1024;
constexpr std::size_t k_max_feeds = 64;

// Control frames — acks, problems, gaps — are owed to the client and wait
// here. State frames never do: they are dropped instead. A queue this deep
// means the socket has stopped taking anything at all.
constexpr std::size_t k_max_queued = 64;

// Bounded so one busy venue cannot hold the pass open indefinitely.
constexpr std::size_t k_max_polls_per_pass = 256;

// One instrument, resolved once at subscribe time: the gateway that answers
// for it, its catalogue row, and the cell every table indexes it by.
struct Target {
    const Gateway* gateway{nullptr};
    const raikiri::Instrument* listed{nullptr};
    std::size_t cell{0};
};

struct EventSource {
    const Gateway* gateway{nullptr};
    shm::ExecReader reader;
};

struct FeedState {
    std::string id;
    Feed kind{Feed::Quotes};
    std::int64_t millihertz{0};
    std::uint64_t period{0};
    MonoNanos due{};
    std::vector<Target> targets;
    ChangeGate gate;
    std::vector<EventSource> sources;
    std::uint64_t seq{0};
};

// A cell that moved: the payload as copied, the instrument that gives it
// meaning, and the version THE COPY carried. The probe's version is not that
// one — the writer may have moved between the two reads, and recording the
// probe's would skip the value it wrote.
template <typename Record> struct Changed {
    std::size_t slot{0};
    std::uint32_t version{0};
    const InstrumentBase* base{nullptr};
    Record record{};
};

// One client, alive as long as its own handler chain holds it.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const Registry& registry, const Clock& clock)
        : ws_{std::move(socket)}, registry_{&registry}, clock_{&clock} {}

    void run() {
        // Read the upgrade request before accepting it, because the path is
        // part of the contract and a socket that asked for something else is
        // not this server's to answer.
        request_ = std::make_unique<http::request_parser<http::empty_body>>();
        auto self = shared_from_this();
        http::async_read(ws_.next_layer(),
                         buffer_,
                         *request_,
                         [self](beast::error_code ec, std::size_t) { self->on_request(ec); });
    }

    // Samples what is due and hands the reactor whatever the socket will take.
    // Returns the frames that left, so an idle caller knows it may sleep.
    std::size_t tick(MonoNanos mono, UnixNanos wall) {
        if (closed_ || !open_) {
            return 0;
        }
        std::size_t sent = drain_events();
        for (auto& feed : feeds_) {
            if (sequenced(feed.kind) || mono.ns < feed.due.ns) {
                continue;
            }
            // The next tick is scheduled from NOW rather than from the missed
            // deadline: a client that stalled is not owed a burst when it
            // comes back.
            feed.due = mono + feed.period;
            if (busy()) {
                continue; // the socket is behind: re-sample next tick, never queue
            }
            sent += sample(feed, mono, wall) ? 1U : 0U;
        }
        return sent;
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }

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

    // Registry hands out gateways by name, so the set of them is the set of
    // account names it reports.
    [[nodiscard]] std::vector<const Gateway*> attached() const {
        std::vector<const Gateway*> out;
        for (const auto& account : registry_->accounts(clock_->mono())) {
            if (const auto* gateway = registry_->find(account.account); gateway != nullptr) {
                out.push_back(gateway);
            }
        }
        return out;
    }

    [[nodiscard]] static std::optional<Target> resolve(const std::vector<const Gateway*>& gateways,
                                                       InstrumentId id) {
        for (const auto* gateway : gateways) {
            // The (venue, market) prefix says which gateway can possibly hold
            // it, so this asks one catalogue rather than every one.
            if (gateway->account.venue != id.venue() || gateway->account.market != id.market()) {
                continue;
            }
            const auto* listed = gateway->channels.catalogue->find(id);
            if (listed == nullptr) {
                continue;
            }
            const std::size_t cell = id.symbol().raw;
            if (cell >= shm::k_bbo_cells) {
                continue; // an id past the tables has nowhere to quote from
            }
            return Target{.gateway = gateway, .listed = listed, .cell = cell};
        }
        return std::nullopt;
    }

    void subscribe(const ClientMessage& message) {
        if (feeds_.size() >= k_max_feeds) {
            enqueue(problem_frame(message.id, {.title = "too many subscriptions", .status = 429}));
            return;
        }
        // A handle is replaced, not duplicated: re-subscribing the same id is
        // how a terminal changes a watchlist.
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
        open_events(message);
    }

    void open_state(const ClientMessage& message, Feed kind, std::span<const InstrumentId> ids) {
        const auto gateways = attached();
        FeedState feed{};
        feed.id = message.id;
        feed.kind = kind;
        std::vector<std::string> unknown;
        for (const auto id : ids) {
            const auto target = resolve(gateways, id);
            if (!target.has_value()) {
                unknown.push_back(digits(id));
                continue;
            }
            feed.targets.push_back(*target);
        }
        if (!unknown.empty()) {
            // Said out loud: a watchlist quietly missing a row is a terminal
            // showing nothing where a price was asked for.
            enqueue(problem_frame(message.id,
                                  {.title = "no gateway holds these instruments",
                                   .status = 404,
                                   .detail = join(unknown)}));
        }
        if (feed.targets.empty()) {
            return;
        }
        // An empty gate and a deadline of now make the first tick a snapshot:
        // nothing has been sent, so everything the tables hold has moved.
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
                // Reconciliation runs in strategyd and reaches Postgres
                // through the journal; no gateway channel carries a verdict,
                // so there is nothing here to sample. Refused rather than
                // acked into silence.
                enqueue(problem_frame(
                    message.id,
                    {.title = "no live channel for reconciliation breaks",
                     .status = 501,
                     .detail = "reconciliation is written by strategyd; read /v1/recon-breaks"}));
                continue;
            case Feed::Orders:
            case Feed::Fills: {
                FeedState feed{};
                feed.id = message.id;
                feed.kind = kind;
                for (const auto* gateway : gateways) {
                    feed.sources.push_back(
                        {.gateway = gateway,
                         .reader = shm::ExecReader{gateway->channels.exec_events}});
                }
                feeds_.push_back(std::move(feed));
                created = true;
                continue;
            }
            case Feed::Positions: {
                FeedState feed{};
                feed.id = message.id;
                feed.kind = kind;
                for (const auto* gateway : gateways) {
                    for (const auto& listed : gateway->channels.catalogue->all()) {
                        const std::size_t cell = base_of(listed).id.symbol().raw;
                        if (cell < shm::k_position_cells) {
                            feed.targets.push_back(
                                {.gateway = gateway, .listed = &listed, .cell = cell});
                        }
                    }
                }
                feed.gate = ChangeGate{feed.targets.size()};
                feed.millihertz =
                    negotiate_millihertz(kind, feed.targets.size(), message.millihertz);
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
        case Feed::Positions:
            return sample_positions(feed, wall);
        case Feed::Health:
            // Gateways only: the database half belongs to whoever owns the
            // connection and this side has none, so `database` here is a
            // default rather than an observation.
            enqueue(health_frame(feed.id, wall, registry_->health(mono)));
            return true;
        case Feed::Orders:
        case Feed::Fills:
        case Feed::Recon:
            return false; // sequenced feeds are drained, never sampled
        }
        return false;
    }

    // Every sampled table is read the same way, and the order matters: probe
    // the version, copy only when it moved, and hand back the version the copy
    // carried so the caller can record it once the frame has actually left.
    template <typename Record, typename Table>
    [[nodiscard]] std::vector<Changed<Record>>
    changed(const FeedState& feed, Table* shm::GatewayChannels::* channel) const {
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

    // Recorded only now, because only now has the frame reached the socket.
    static void remember(FeedState& feed,
                         const std::vector<std::pair<std::size_t, std::uint32_t>>& taken) {
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
        enqueue(book_frame(feed.id, wall, book_of(cell.record, *cell.base)));
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
                    // Ring ITEMS, not frames of this subscription: the events
                    // are gone, so how many of them this feed would have sent
                    // is exactly what cannot be known.
                    enqueue(gap_frame(feed.id, source.reader.gap()));
                    ++sent;
                    break;
                case shm::PollResult::Item: {
                    any = true;
                    if (feed.kind == Feed::Fills && record.event.type != OrderEventType::Filled) {
                        break; // not this subscription's event, and nothing was lost
                    }
                    const auto* listed =
                        source.gateway->channels.catalogue->find(record.event.instrument);
                    if (listed == nullptr) {
                        break; // a price without its instrument is not a price
                    }
                    ++feed.seq;
                    const auto& base = base_of(*listed);
                    enqueue(feed.kind == Feed::Fills
                                ? fill_frame(feed.id, feed.seq, record.event, base)
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
            // Events drain at the speed of the socket rather than of the tick,
            // because a fill that waited a poll cycle is a fill nobody saw.
            self->drain_events();
            self->pump();
        });
    }

    websock::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::unique_ptr<http::request_parser<http::empty_body>> request_;
    const Registry* registry_;
    const Clock* clock_;
    std::vector<FeedState> feeds_;
    std::deque<std::string> out_;
    std::string writing_buf_;
    bool writing_{false};
    bool open_{false};
    bool closed_{false};
};

} // namespace

struct Stream::Impl {
    Impl(const Registry& reg, const Clock& clk, std::string addr, std::uint16_t p)
        : registry{reg}, clock{clk}, address{std::move(addr)}, port{p}, acceptor{ioc} {}

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

    const Registry& registry;
    const Clock& clock;
    std::string address;
    std::uint16_t port;
    asio::io_context ioc{1};
    tcp::acceptor acceptor;
    std::vector<std::shared_ptr<Session>> sessions;
    bool running{false};
};

Stream::Stream(const Registry& registry,
               const Clock& clock,
               std::string address,
               std::uint16_t port)
    : impl_{std::make_unique<Impl>(registry, clock, std::move(address), port)} {}

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
    // Port 0 asks the kernel to choose, which is how a test binds without
    // agreeing a number with whatever else is running.
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
        std::erase_if(impl_->sessions,
                      [](const std::shared_ptr<Session>& session) { return session->closed(); });
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

std::uint16_t Stream::port() const noexcept {
    return impl_->port;
}

std::size_t Stream::clients() const noexcept {
    return impl_->sessions.size();
}

} // namespace raikiri::admin
