#include <raikiri/admin/format.hpp>
#include <raikiri/admin/history.hpp>
#include <raikiri/domain/order.hpp>

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <pqxx/pqxx>
#include <string_view>
#include <utility>

namespace raikiri::admin {

namespace {

// Which statuses can still fill. The same set as the partial index in
// sql/migrations/0001_init.sql, spelled the same way so the `open` filter uses
// it; `initialized` is absent because nothing was sent to a venue yet.
constexpr std::string_view k_open_statuses =
    "('submitted','accepted','partially_filled','pending_update','pending_cancel','triggered')";

// persisterd writes this exact sentence for the Unknown verdict
// (libs/persist/src/mapper.cpp). recon_breaks has no verdict column, so the
// verdict is recovered from it — and `unknown` is the venue not answering,
// which is NOT agreement.
constexpr std::string_view k_unknown_detail = "venue could not report";

// Every instrument of one (venue, market) falls in a span this wide: the packed
// word puts the market byte at bit 48 and everything below it names the symbol.
constexpr std::uint64_t k_symbol_span = 1ULL << 48;

constexpr char k_cursor_separator = '\x1F';

[[nodiscard]] std::string to_hex(std::string_view raw) {
    constexpr std::string_view digits_16 = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(digits_16[byte >> 4U]);
        out.push_back(digits_16[byte & 0x0FU]);
    }
    return out;
}

[[nodiscard]] std::optional<std::string> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0;
        const auto* stop = text.data() + i + 2;
        if (std::from_chars(text.data() + i, stop, value, 16).ptr != stop) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>(value));
    }
    return out;
}

[[nodiscard]] std::optional<std::uint64_t> as_unsigned(std::string_view text) {
    std::uint64_t value = 0;
    const auto* stop = text.data() + text.size();
    const auto [end, ec] = std::from_chars(text.data(), stop, value);
    if (ec != std::errc{} || end != stop) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::uint64_t digits_to_unsigned(std::string_view text) {
    return as_unsigned(text).value_or(0);
}

// A condition and the values it binds are appended by one call, so a
// placeholder number cannot drift away from the value it stands for.
class Where {
public:
    // `expr op $n`, or `expr op ($n,$n+1,…)` for a row-value comparison.
    template <typename... Values>
    void add(std::string_view expr, std::string_view op, Values&&... values) {
        static_assert(sizeof...(Values) >= 1);
        begin();
        sql_ += expr;
        sql_ += ' ';
        sql_ += op;
        sql_ += ' ';
        if constexpr (sizeof...(Values) > 1) {
            sql_ += '(';
        }
        bool first = true;
        const auto bind = [&](auto&& value) {
            if (!std::exchange(first, false)) {
                sql_ += ',';
            }
            sql_ += placeholder(std::forward<decltype(value)>(value));
        };
        (bind(std::forward<Values>(values)), ...);
        if constexpr (sizeof...(Values) > 1) {
            sql_ += ')';
        }
    }

    // The (venue, market) prefix of a packed instrument id, as a half-open
    // range. A range predicate can use the index on instrument_id; the shift
    // expression that reads the same bits cannot.
    //
    // `nullable` keeps a row whose instrument this system never learned: it is
    // not excluded by a market filter it cannot be tested against, because
    // dropping it would hide a position rather than fail to classify one.
    void add_account(std::string_view column, AccountId account, bool nullable) {
        const auto lo = InstrumentId::make(account.venue, account.market, SymbolId{0}).packed;
        begin();
        sql_ += '(';
        if (nullable) {
            sql_ += column;
            sql_ += " IS NULL OR ";
        }
        sql_ += column;
        sql_ += " >= ";
        sql_ += placeholder(static_cast<std::int64_t>(lo));
        sql_ += " AND ";
        sql_ += column;
        sql_ += " < ";
        sql_ += placeholder(static_cast<std::int64_t>(lo + k_symbol_span));
        sql_ += ')';
    }

    // A condition with no parameters. Only ever built from literals in this
    // file, never from anything a client sent.
    void raw(std::string_view condition) {
        begin();
        sql_ += condition;
    }

    [[nodiscard]] const std::string& text() const noexcept { return sql_; }

    [[nodiscard]] const pqxx::params& params() const noexcept { return params_; }

private:
    void begin() { sql_ += sql_.empty() ? " WHERE " : " AND "; }

    template <typename T> [[nodiscard]] std::string placeholder(T&& value) {
        params_.append(std::forward<T>(value));
        return "$" + digits(++count_);
    }

    std::string sql_;
    pqxx::params params_;
    std::uint64_t count_{0};
};

[[nodiscard]] std::string text_of(pqxx::field_ref field) {
    return field.is_null() ? std::string{} : std::string{field.view()};
}

[[nodiscard]] std::optional<std::string> opt_text(pqxx::field_ref field) {
    if (field.is_null() || field.view().empty()) {
        return std::nullopt;
    }
    return std::string{field.view()};
}

[[nodiscard]] std::int64_t int_of(pqxx::field_ref field) {
    return field.is_null() ? 0 : field.as<std::int64_t>();
}

[[nodiscard]] std::optional<std::int64_t> opt_int(pqxx::field_ref field) {
    return field.is_null() ? std::nullopt : std::optional{field.as<std::int64_t>()};
}

// Ids and nanosecond timestamps, which are the same problem: both are past
// 2^53 and both are stored in a BIGINT. Read as signed and rendered unsigned,
// because the packed word of every venue this system knows sits well below
// 2^63 and reading it as unsigned would throw on a row that somehow did not.
[[nodiscard]] std::string digits_of(pqxx::field_ref field) {
    return digits(static_cast<std::uint64_t>(int_of(field)));
}

// Zero is absence, not an id: venue 0 is Invalid and a client order id of zero
// never left a strategy, and both are what persisterd writes for "unknown".
[[nodiscard]] std::optional<std::string> opt_id(pqxx::field_ref field) {
    const auto value = opt_int(field);
    if (!value.has_value() || *value == 0) {
        return std::nullopt;
    }
    return digits(static_cast<std::uint64_t>(*value));
}

// A NULL tick or lot count is no value, not a count of zero: a market order
// carries no price, and rendering the zero would put one on it.
[[nodiscard]] PriceTicks ticks_of(pqxx::field_ref field) {
    const auto value = opt_int(field);
    return value.has_value() ? PriceTicks::from_raw(*value) : PriceTicks::undef();
}

[[nodiscard]] QtyLots lots_of(pqxx::field_ref field) {
    const auto value = opt_int(field);
    return value.has_value() ? QtyLots::from_raw(*value) : QtyLots::undef();
}

// Money is already 1e9 fixed-point and needs no instrument. Zero is a real
// amount — a fee of nothing is a fee — so only the undefined sentinel is
// absent.
[[nodiscard]] std::optional<std::string> opt_amount(pqxx::field_ref field) {
    const auto raw = opt_int(field);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    auto text = decimal(FixedE9::from_raw(*raw));
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] FixedE9 scale_of(pqxx::field_ref field) {
    const auto value = opt_int(field);
    return value.has_value() ? FixedE9::from_raw(*value) : FixedE9::undef();
}

[[nodiscard]] Scales scales_of(pqxx::field_ref tick, pqxx::field_ref lot) {
    return {.tick_size = scale_of(tick), .lot_size = scale_of(lot)};
}

[[nodiscard]] Order read_order(pqxx::row_ref row) {
    const auto scales = scales_of(row.at("tick_size_e9"), row.at("lot_size_e9"));
    const ClientOrderId coid{static_cast<std::uint64_t>(int_of(row.at("client_order_id")))};
    const auto flags = static_cast<OrderFlags>(static_cast<std::uint8_t>(int_of(row.at("flags"))));
    return {
        .client_order_id = digits(coid.raw),
        .exchange_client_order_id = opt_text(row.at("exchange_client_order_id")),
        .exchange_order_id = opt_text(row.at("exchange_order_id")),
        .instrument_id = digits_of(row.at("instrument_id")),
        .strategy_id = coid.strategy_id(),
        .side = text_of(row.at("side")),
        .order_type = text_of(row.at("order_type")),
        .tif = text_of(row.at("tif")),
        .post_only = has_flag(flags, OrderFlags::PostOnly),
        .reduce_only = has_flag(flags, OrderFlags::ReduceOnly),
        .price = scales.price(ticks_of(row.at("price_ticks"))),
        .qty = scales.size(lots_of(row.at("qty_lots"))).value_or(std::string{}),
        .leaves = scales.size(lots_of(row.at("leaves_lots"))),
        .status = text_of(row.at("status")),
        .reject_class = opt_text(row.at("reject_class")),
        .reject_code = opt_int(row.at("reject_code")),
        .ts_created = digits_of(row.at("ts_created")),
        .ts_updated = digits_of(row.at("ts_updated")),
    };
}

[[nodiscard]] OrderEvent read_order_event(pqxx::row_ref row) {
    const auto scales = scales_of(row.at("tick_size_e9"), row.at("lot_size_e9"));
    return {
        .client_order_id = digits_of(row.at("client_order_id")),
        .instrument_id = digits_of(row.at("instrument_id")),
        .event_type = text_of(row.at("event_type")),
        .price = scales.price(ticks_of(row.at("price_ticks"))),
        .qty = scales.size(lots_of(row.at("qty_lots"))),
        .leaves = scales.size(lots_of(row.at("leaves_lots"))),
        .fee = opt_amount(row.at("fee_e9")),
        .exec_id = opt_text(row.at("exec_id")),
        .ts_event = digits_of(row.at("ts_event")),
    };
}

[[nodiscard]] Fill read_fill(pqxx::row_ref row) {
    const auto scales = scales_of(row.at("tick_size_e9"), row.at("lot_size_e9"));
    return {
        .venue = text_of(row.at("venue")),
        .exec_id = text_of(row.at("exec_id")),
        .client_order_id = opt_id(row.at("client_order_id")),
        .exchange_order_id = opt_text(row.at("exchange_order_id")),
        .instrument_id = digits_of(row.at("instrument_id")),
        .side = text_of(row.at("side")),
        .price = scales.price(ticks_of(row.at("price_ticks"))).value_or(std::string{}),
        .qty = scales.size(lots_of(row.at("qty_lots"))).value_or(std::string{}),
        .fee = opt_amount(row.at("fee_e9")),
        .liquidity = text_of(row.at("liquidity")),
        .ts_event = digits_of(row.at("ts_event")),
    };
}

[[nodiscard]] Position read_position(pqxx::row_ref row) {
    const Scales scales{.lot_size = scale_of(row.at("lot_size_e9"))};
    Position out{};
    out.instrument_id = opt_id(row.at("instrument_id")).value_or(std::string{});
    out.strategy_id = int_of(row.at("strategy_id"));
    out.qty = scales.size(lots_of(row.at("net_qty_lots"))).value_or(std::string{});
    out.realized = opt_amount(row.at("realized_e9")).value_or(std::string{});
    // avg_entry_price stays unset: cost over quantity is arithmetic that
    // raikiri::portfolio::Position already owns, and this library does not link
    // it. A second derivation of an entry price is how two views of one book
    // start disagreeing.
    //
    // venue_qty and venue_reported_at stay unset for a stronger reason: NOTHING
    // RECORDS THEM. recon_breaks holds what a venue said only when it
    // disagreed, and reading a stale disagreement as "what the venue holds"
    // would answer a question the tables cannot.
    out.ts_updated = digits_of(row.at("ts_updated"));
    return out;
}

[[nodiscard]] PnlIncrement read_pnl(pqxx::row_ref row) {
    return {
        .source_id = text_of(row.at("source_id")),
        .account = text_of(row.at("account_id")),
        .kind = text_of(row.at("kind")),
        .instrument_id = opt_id(row.at("instrument_id")),
        .strategy_id = int_of(row.at("strategy_id")),
        .amount = opt_amount(row.at("amount_e9")).value_or(std::string{}),
        .ts_event = digits_of(row.at("ts_event")),
    };
}

[[nodiscard]] FundingObservation read_funding(pqxx::row_ref row) {
    const Scales scales{.tick_size = scale_of(row.at("tick_size_e9"))};
    // Zero is not a time and not an interval: the journal writes zero where the
    // venue said nothing, and a next funding time of zero is a date in 1970
    // while an interval of zero divides.
    const auto next = int_of(row.at("next_funding_ts"));
    const auto interval = int_of(row.at("interval_s"));
    return {
        .instrument_id = digits_of(row.at("instrument_id")),
        .rate = opt_amount(row.at("rate_e9")).value_or(std::string{}),
        .predicted_rate = opt_amount(row.at("predicted_e9")),
        .mark_price = scales.price(ticks_of(row.at("mark_ticks"))),
        .next_funding_time =
            next > 0 ? std::optional{digits(static_cast<std::uint64_t>(next))} : std::nullopt,
        .interval_seconds = interval > 0 ? std::optional{interval} : std::nullopt,
        .ts_event = digits_of(row.at("ts_event")),
    };
}

[[nodiscard]] ReconBreak read_recon_break(pqxx::row_ref row) {
    // (venue, exchange_symbol) is unique per market_type and a break records no
    // market, so two listings of one symbol leave the instrument unresolvable.
    // Naming one of them would be a coin toss written down as a fact.
    const bool resolved = int_of(row.at("matches")) == 1;
    const Scales scales{.lot_size = resolved ? scale_of(row.at("lot_size_e9")) : FixedE9::undef()};
    const auto detail = text_of(row.at("detail"));
    return {
        .venue = text_of(row.at("venue")),
        .instrument_id = resolved ? opt_id(row.at("instrument_id")) : std::nullopt,
        .symbol = text_of(row.at("exchange_symbol")),
        .verdict = detail == k_unknown_detail ? "unknown" : "break",
        .local_qty = scales.size(lots_of(row.at("local_qty_lots"))),
        .venue_qty = scales.size(lots_of(row.at("venue_qty_lots"))),
        .detail = detail,
        .ts_seen = digits_of(row.at("ts_seen")),
    };
}

[[nodiscard]] std::size_t clamp_limit(std::size_t asked) noexcept {
    return std::clamp<std::size_t>(asked, 1, k_max_page);
}

// One row past the page is asked for, and it is the proof that a next page
// exists rather than part of the answer.
[[nodiscard]] std::string limit_clause(std::size_t limit) {
    return " LIMIT " + digits(limit + 1);
}

template <typename T, typename Read>
[[nodiscard]] Page<T> fetch(pqxx::connection& conn,
                            const std::string& sql,
                            const pqxx::params& params,
                            std::size_t limit,
                            Read read) {
    pqxx::read_transaction tx{conn};
    const auto rows = tx.exec(sql, params);
    tx.commit();

    Page<T> page;
    page.items.reserve(limit);
    for (const auto row : rows) {
        if (page.items.size() == limit) {
            page.next_cursor = cursor_after(page.items.back()).encode();
            break;
        }
        page.items.push_back(read(row));
    }
    return page;
}

} // namespace

// -------------------------------------------------------------- units ----

std::optional<std::string> Scales::price(PriceTicks ticks) const {
    if (tick_size.is_undef() || tick_size.raw <= 0 || ticks.is_undef()) {
        return std::nullopt;
    }
    // The domain's arithmetic aborts on overflow, which is right for numbers
    // this system computed and wrong for a catalogue row it merely read.
    const auto magnitude = ticks.raw < 0 ? -ticks.raw : ticks.raw;
    if (magnitude > std::numeric_limits<std::int64_t>::max() / tick_size.raw) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.tick_size = tick_size;
    return admin::price(ticks, base);
}

std::optional<std::string> Scales::size(QtyLots lots) const {
    if (lot_size.is_undef() || lot_size.raw <= 0 || lots.is_undef()) {
        return std::nullopt;
    }
    const auto magnitude = lots.raw < 0 ? -lots.raw : lots.raw;
    if (magnitude > std::numeric_limits<std::int64_t>::max() / lot_size.raw) {
        return std::nullopt;
    }
    InstrumentBase base{};
    base.lot_size = lot_size;
    return admin::size(lots, base);
}

// ------------------------------------------------------------- cursors ----

std::string Cursor::encode() const {
    std::string raw = digits(ts);
    raw += k_cursor_separator;
    raw += key;
    raw += k_cursor_separator;
    raw += key2;
    return to_hex(raw);
}

std::optional<Cursor> Cursor::decode(std::string_view text) {
    const auto raw = from_hex(text);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const std::string_view view{*raw};
    const auto first = view.find(k_cursor_separator);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = view.find(k_cursor_separator, first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    // A separator inside a key would split the cursor into four parts. No venue
    // symbol or execution id holds a control byte, so that is a forged cursor
    // and it is refused rather than half-read.
    if (view.find(k_cursor_separator, second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto ts = as_unsigned(view.substr(0, first));
    if (!ts.has_value()) {
        return std::nullopt;
    }
    return Cursor{.ts = *ts,
                  .key = std::string{view.substr(first + 1, second - first - 1)},
                  .key2 = std::string{view.substr(second + 1)}};
}

Cursor cursor_after(const Order& last) {
    return {.ts = digits_to_unsigned(last.ts_created), .key = last.client_order_id, .key2 = {}};
}

Cursor cursor_after(const Fill& last) {
    return {.ts = digits_to_unsigned(last.ts_event), .key = last.venue, .key2 = last.exec_id};
}

Cursor cursor_after(const PnlIncrement& last) {
    return {.ts = digits_to_unsigned(last.ts_event), .key = last.source_id, .key2 = {}};
}

Cursor cursor_after(const FundingObservation& last) {
    return {.ts = digits_to_unsigned(last.ts_event), .key = last.instrument_id, .key2 = {}};
}

Cursor cursor_after(const ReconBreak& last) {
    return {.ts = digits_to_unsigned(last.ts_seen), .key = last.venue, .key2 = last.symbol};
}

std::string_view pnl_kind_name(PnlKind kind) noexcept {
    switch (kind) {
    case PnlKind::Realized:
        return "realized";
    case PnlKind::Fee:
        return "fee";
    case PnlKind::Funding:
        return "funding";
    }
    return "realized";
}

std::optional<PnlKind> pnl_kind_from_string(std::string_view text) noexcept {
    if (text == "realized") {
        return PnlKind::Realized;
    }
    if (text == "fee") {
        return PnlKind::Fee;
    }
    if (text == "funding") {
        return PnlKind::Funding;
    }
    return std::nullopt;
}

// ------------------------------------------------------------ database ----

struct History::Impl {
    std::string dsn;
    std::unique_ptr<pqxx::connection> conn;

    explicit Impl(std::string d) : dsn{std::move(d)} { (void)open(); }

    // Opened on demand and left absent while Postgres is down, so admind keeps
    // answering from shared memory and picks history back up by itself.
    pqxx::connection* open() noexcept {
        if (conn != nullptr && conn->is_open()) {
            return conn.get();
        }
        try {
            conn = std::make_unique<pqxx::connection>(dsn);
        } catch (const std::exception&) {
            conn.reset();
        }
        return conn.get();
    }

    // Every read is best-effort. A handler that threw would become a 500 for a
    // question the live half of the API could still have answered.
    template <typename T, typename Fn> T read(T fallback, Fn&& fn) {
        auto* live = open();
        if (live == nullptr) {
            return fallback;
        }
        try {
            return fn(*live);
        } catch (const std::exception&) {
            conn.reset(); // a broken connection must stop claiming to be one
            return fallback;
        }
    }
};

History::History(std::string dsn) : impl_{std::make_unique<Impl>(std::move(dsn))} {}

History::~History() = default;

bool History::connected() const noexcept {
    return impl_->conn != nullptr && impl_->conn->is_open();
}

DatabaseHealth History::health() const {
    return impl_->read(DatabaseHealth{}, [](pqxx::connection& conn) {
        pqxx::read_transaction tx{conn};
        // How long since persisterd last committed a batch. That is not lag on
        // an idle journal, and it is the closest this side can measure: the
        // journal head is a file on the gateway's host, not a row here.
        const auto rows = tx.exec("SELECT EXTRACT(EPOCH FROM (now() - MAX(updated_at)))"
                                  " FROM raikiri.persist_cursors");
        tx.commit();
        DatabaseHealth out{};
        out.reachable = true;
        if (!rows.empty() && !rows[0][0].is_null()) {
            out.lag_seconds = rows[0][0].as<double>();
        }
        return out;
    });
}

// ------------------------------------------------------------- trading ----

namespace {

constexpr std::string_view k_order_select =
    "SELECT o.client_order_id, o.exchange_client_order_id, o.exchange_order_id,"
    " o.instrument_id, o.side, o.order_type, o.tif, o.flags, o.price_ticks, o.qty_lots,"
    " o.leaves_lots, o.status, o.reject_class, o.reject_code,"
    " o.ts_created, o.ts_updated, i.tick_size_e9, i.lot_size_e9"
    " FROM raikiri.orders o"
    " LEFT JOIN raikiri.instruments i ON i.instrument_id = o.instrument_id";

constexpr std::string_view k_fill_select =
    "SELECT x.venue, x.exec_id, x.exchange_order_id, x.client_order_id, x.instrument_id,"
    " x.side, x.price_ticks, x.qty_lots, x.fee_e9, x.liquidity, x.ts_event,"
    " i.tick_size_e9, i.lot_size_e9"
    " FROM raikiri.executions x"
    " LEFT JOIN raikiri.instruments i ON i.instrument_id = x.instrument_id";

} // namespace

OrderPage History::orders(const PageQuery& query, std::optional<bool> open) const {
    const auto limit = clamp_limit(query.limit);
    Where where;
    if (query.account.has_value()) {
        where.add_account("o.instrument_id", *query.account, false);
    }
    if (query.instrument.has_value()) {
        where.add("o.instrument_id", "=", static_cast<std::int64_t>(query.instrument->packed));
    }
    if (query.since.has_value()) {
        where.add("o.ts_created", ">=", static_cast<std::int64_t>(query.since->ns));
    }
    if (open.has_value()) {
        where.raw(std::string{*open ? "o.status IN " : "o.status NOT IN "} +
                  std::string{k_open_statuses});
    }
    if (query.cursor.has_value()) {
        const auto key = as_unsigned(query.cursor->key);
        if (!key.has_value()) {
            return {}; // a cursor whose key is not the number this column holds
        }
        where.add("(o.ts_created, o.client_order_id)",
                  "<",
                  static_cast<std::int64_t>(query.cursor->ts),
                  static_cast<std::int64_t>(*key));
    }

    // ts_created, never ts_updated: a keyset cursor over a column that moves
    // under a live order would skip the rows that moved past it.
    const auto sql = std::string{k_order_select} + where.text() +
                     " ORDER BY o.ts_created DESC, o.client_order_id DESC" + limit_clause(limit);
    return impl_->read(OrderPage{}, [&](pqxx::connection& conn) {
        return fetch<Order>(conn, sql, where.params(), limit, read_order);
    });
}

std::optional<Order> History::order(ClientOrderId id) const {
    const auto sql = std::string{k_order_select} + " WHERE o.client_order_id = $1";
    pqxx::params params;
    params.append(static_cast<std::int64_t>(id.raw));
    return impl_->read(std::optional<Order>{}, [&](pqxx::connection& conn) {
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec(sql, params);
        tx.commit();
        return rows.empty() ? std::nullopt : std::optional{read_order(rows[0])};
    });
}

std::vector<OrderEvent> History::order_events(ClientOrderId id) const {
    // Oldest first, and by the journal that recorded them rather than by the
    // venue's clock alone: two events of one order can share a nanosecond, and
    // (journal, journal_seq) is the order they were durably written in.
    const auto sql =
        "SELECT e.client_order_id, e.instrument_id, e.event_type, e.price_ticks, e.qty_lots,"
        " e.leaves_lots, e.fee_e9, e.exec_id, e.ts_event, i.tick_size_e9, i.lot_size_e9"
        " FROM raikiri.order_events e"
        " LEFT JOIN raikiri.instruments i ON i.instrument_id = e.instrument_id"
        " WHERE e.client_order_id = $1"
        " ORDER BY e.ts_event, e.journal, e.journal_seq"
        " LIMIT " +
        digits(k_max_page);
    pqxx::params params;
    params.append(static_cast<std::int64_t>(id.raw));
    return impl_->read(std::vector<OrderEvent>{}, [&](pqxx::connection& conn) {
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec(sql, params);
        tx.commit();
        std::vector<OrderEvent> out;
        out.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto row : rows) {
            out.push_back(read_order_event(row));
        }
        return out;
    });
}

FillPage History::fills(const PageQuery& query) const {
    const auto limit = clamp_limit(query.limit);
    Where where;
    if (query.account.has_value()) {
        where.add_account("x.instrument_id", *query.account, false);
    }
    if (query.instrument.has_value()) {
        where.add("x.instrument_id", "=", static_cast<std::int64_t>(query.instrument->packed));
    }
    if (query.since.has_value()) {
        where.add("x.ts_event", ">=", static_cast<std::int64_t>(query.since->ns));
    }
    if (query.cursor.has_value()) {
        where.add("(x.ts_event, x.venue, x.exec_id)",
                  "<",
                  static_cast<std::int64_t>(query.cursor->ts),
                  query.cursor->key,
                  query.cursor->key2);
    }

    const auto sql = std::string{k_fill_select} + where.text() +
                     " ORDER BY x.ts_event DESC, x.venue DESC, x.exec_id DESC" +
                     limit_clause(limit);
    return impl_->read(FillPage{}, [&](pqxx::connection& conn) {
        return fetch<Fill>(conn, sql, where.params(), limit, read_fill);
    });
}

std::vector<Position> History::positions(std::optional<AccountId> account,
                                         std::optional<std::uint16_t> strategy) const {
    Where where;
    if (account.has_value()) {
        where.add("p.venue", "=", std::string{to_string(account->venue)});
        where.add_account("p.instrument_id", *account, true);
    }
    if (strategy.has_value()) {
        where.add("p.strategy_id", "=", static_cast<std::int64_t>(*strategy));
    }

    const auto sql = "SELECT p.strategy_id, p.venue, p.exchange_symbol, p.instrument_id,"
                     " p.net_qty_lots, p.realized_e9, p.ts_updated, i.lot_size_e9"
                     " FROM raikiri.positions p"
                     " LEFT JOIN raikiri.instruments i ON i.instrument_id = p.instrument_id" +
                     where.text() + " ORDER BY p.venue, p.exchange_symbol, p.strategy_id";
    return impl_->read(std::vector<Position>{}, [&](pqxx::connection& conn) {
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec(sql, where.params());
        tx.commit();
        std::vector<Position> out;
        out.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto row : rows) {
            out.push_back(read_position(row));
        }
        return out;
    });
}

// ----------------------------------------------------------- analytics ----

PnlPage History::pnl(const PageQuery& query, std::optional<PnlKind> kind) const {
    const auto limit = clamp_limit(query.limit);
    Where where;
    if (query.account.has_value()) {
        // account_id holds the venue, which is what persisterd writes into it;
        // the market is only recoverable from the nullable instrument_id.
        where.add("p.venue", "=", std::string{to_string(query.account->venue)});
        where.add_account("p.instrument_id", *query.account, true);
    }
    if (kind.has_value()) {
        where.add("p.kind", "=", std::string{pnl_kind_name(*kind)});
    }
    if (query.since.has_value()) {
        where.add("p.ts_event", ">=", static_cast<std::int64_t>(query.since->ns));
    }
    if (query.cursor.has_value()) {
        where.add("(p.ts_event, p.source_id)",
                  "<",
                  static_cast<std::int64_t>(query.cursor->ts),
                  query.cursor->key);
    }

    const auto sql = "SELECT p.source_id, p.account_id, p.kind, p.amount_e9, p.ts_event,"
                     " p.strategy_id, p.instrument_id"
                     " FROM raikiri.pnl_increments p" +
                     where.text() + " ORDER BY p.ts_event DESC, p.source_id DESC" +
                     limit_clause(limit);
    return impl_->read(PnlPage{}, [&](pqxx::connection& conn) {
        return fetch<PnlIncrement>(conn, sql, where.params(), limit, read_pnl);
    });
}

FundingPage History::funding(const PageQuery& query) const {
    const auto limit = clamp_limit(query.limit);
    Where where;
    if (query.account.has_value()) {
        where.add_account("f.instrument_id", *query.account, false);
    }
    if (query.instrument.has_value()) {
        where.add("f.instrument_id", "=", static_cast<std::int64_t>(query.instrument->packed));
    }
    if (query.since.has_value()) {
        where.add("f.ts_event", ">=", static_cast<std::int64_t>(query.since->ns));
    }
    if (query.cursor.has_value()) {
        const auto key = as_unsigned(query.cursor->key);
        if (!key.has_value()) {
            return {};
        }
        where.add("(f.ts_event, f.instrument_id)",
                  "<",
                  static_cast<std::int64_t>(query.cursor->ts),
                  static_cast<std::int64_t>(*key));
    }

    const auto sql = "SELECT f.instrument_id, f.ts_event, f.rate_e9, f.predicted_e9, f.mark_ticks,"
                     " f.next_funding_ts, f.interval_s, i.tick_size_e9"
                     " FROM raikiri.funding_observations f"
                     " LEFT JOIN raikiri.instruments i ON i.instrument_id = f.instrument_id" +
                     where.text() + " ORDER BY f.ts_event DESC, f.instrument_id DESC" +
                     limit_clause(limit);
    return impl_->read(FundingPage{}, [&](pqxx::connection& conn) {
        return fetch<FundingObservation>(conn, sql, where.params(), limit, read_funding);
    });
}

ReconBreakPage History::recon_breaks(const PageQuery& query) const {
    const auto limit = clamp_limit(query.limit);
    Where where;
    if (query.account.has_value()) {
        // A break records (venue, symbol) and no market, so an account narrows
        // this to its venue and no further.
        where.add("b.venue", "=", std::string{to_string(query.account->venue)});
    }
    if (query.since.has_value()) {
        where.add("b.ts_seen", ">=", static_cast<std::int64_t>(query.since->ns));
    }
    if (query.cursor.has_value()) {
        where.add("(b.ts_seen, b.venue, b.exchange_symbol)",
                  "<",
                  static_cast<std::int64_t>(query.cursor->ts),
                  query.cursor->key,
                  query.cursor->key2);
    }

    const auto sql =
        "SELECT b.venue, b.exchange_symbol, b.ts_seen, b.local_qty_lots, b.venue_qty_lots,"
        " b.detail, m.instrument_id, m.lot_size_e9, m.matches"
        " FROM raikiri.recon_breaks b"
        " LEFT JOIN LATERAL (SELECT MIN(i.instrument_id) AS instrument_id,"
        "                           MIN(i.lot_size_e9) AS lot_size_e9, COUNT(*) AS matches"
        "                      FROM raikiri.instruments i"
        "                     WHERE i.venue = b.venue"
        "                       AND i.exchange_symbol = b.exchange_symbol) m ON TRUE" +
        where.text() + " ORDER BY b.ts_seen DESC, b.venue DESC, b.exchange_symbol DESC" +
        limit_clause(limit);
    return impl_->read(ReconBreakPage{}, [&](pqxx::connection& conn) {
        return fetch<ReconBreak>(conn, sql, where.params(), limit, read_recon_break);
    });
}

} // namespace raikiri::admin
