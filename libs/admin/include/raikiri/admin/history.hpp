#pragma once

// The cold half of admind: what persisterd has written to Postgres.
//
// Nothing live comes through here. A gateway's shared memory answers "now",
// this answers "what happened", and mixing them would put a database on the
// path of a question that has a faster true answer.
//
// The shapes below are the CONTRACT's, not the tables': a price stops being a
// tick count and an id stops being a packed word, because both are past 2^53
// and a browser holds integers exactly only to there. schema/admin-api.yaml
// describes every field, and is the only place that describes them.

#include <raikiri/admin/types.hpp>
#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::admin {

inline constexpr std::size_t k_default_page = 200;
inline constexpr std::size_t k_max_page = 1000;

// One row's ticks and lots resolved through the instrument that gives them
// meaning. Undefined when the row's instrument is not in raikiri.instruments —
// which is a live state, since that table is loaded by ops tooling while
// persisterd writes orders regardless — and then the row renders no price and
// no size at all. A tick count emitted as a price is wrong by whatever the
// tick size is, and wrong by four orders of magnitude is the usual case.
struct Scales {
    FixedE9 tick_size{FixedE9::undef()};
    FixedE9 lot_size{FixedE9::undef()};

    [[nodiscard]] std::optional<std::string> price(PriceTicks ticks) const;
    [[nodiscard]] std::optional<std::string> size(QtyLots lots) const;

    // Each undefined half taken from `other`.
    [[nodiscard]] Scales or_from(const Scales& other) const noexcept;
};

// The attached gateways' catalogue, asked when raikiri.instruments has no row.
using ScaleLookup = std::function<Scales(InstrumentId)>;

// Keyset pagination. CURSORS, NOT OFFSETS: rows arrive continuously and an
// offset walks a moving list, so a page boundary names the last row it emitted
// and the next page resumes strictly after it.
struct Cursor {
    std::uint64_t ts{0}; // the ordering timestamp of the last row emitted
    std::string key;     // that row's tiebreak, as text
    std::string key2;    // a second tiebreak, empty where one suffices

    // Opaque by contract, so encoded rather than readable: a client that learns
    // to read a cursor starts constructing one, and the day the ordering key
    // changes its pages move without anyone saying so.
    [[nodiscard]] std::string encode() const;
    [[nodiscard]] static std::optional<Cursor> decode(std::string_view text);

    friend bool operator==(const Cursor&, const Cursor&) = default;
};

// ------------------------------------------------------------ trading ----

struct Order {
    std::string client_order_id;
    std::optional<std::string> exchange_client_order_id;
    std::optional<std::string> exchange_order_id;
    std::string instrument_id;
    std::int64_t strategy_id{0};
    std::string side;
    std::string order_type;
    std::string tif;
    bool post_only{false};
    bool reduce_only{false};
    std::optional<std::string> price; // absent on a market order
    std::string qty;
    // What is still working. The filled amount is the DROP in this between two
    // events, never an event's own qty: a venue that resends a fill
    // notification would otherwise book it twice.
    std::optional<std::string> leaves;
    std::string status;
    // WHY, when the status is a rejection. Absent on every other order, and on
    // one refused before the journal carried a reason — which is not the same
    // as the venue having given none, and a reader can tell them apart because
    // these are absent rather than empty.
    //
    // `reject_code` is the VENUE'S OWN number, unchanged. -1058 is a thing a
    // person can look up; "terminal" alone is not.
    std::optional<std::string> reject_class;
    std::optional<std::int64_t> reject_code;
    std::string ts_created;
    std::string ts_updated;
};

struct OrderEvent {
    std::string client_order_id;
    std::string instrument_id;
    std::string event_type;
    std::optional<std::string> price;
    std::optional<std::string> qty;
    std::optional<std::string> leaves;
    std::optional<std::string> fee;
    std::optional<std::string> exec_id;
    std::string ts_event;
};

struct Fill {
    std::string venue;
    std::string exec_id; // the venue's own idea of unique; dedup keys on it
    std::optional<std::string> client_order_id;
    std::optional<std::string> exchange_order_id;
    std::string instrument_id;
    std::string side;
    std::string price;
    std::string qty;
    std::optional<std::string> fee;
    std::string liquidity;
    std::string ts_event;
};

// --------------------------------------------------------- analytics ----

struct PnlIncrement {
    std::string source_id; // the idempotency key the ledger dedupes on
    std::string account;
    std::string kind;
    std::optional<std::string> instrument_id;
    std::int64_t strategy_id{0};
    std::string amount; // signed; positive is money gained
    std::string ts_event;
};

// The contract's FundingObservation, which is deliberately not the live
// Funding of types.hpp: a stored observation is mostly holes — few venues
// publish a predicted rate and none of them publish all of these — and the
// answer for a field with no answer is absence, not an empty string.
struct FundingObservation {
    std::string instrument_id;
    std::string rate; // a ratio per interval, not a percent and not an APR
    std::optional<std::string> predicted_rate;
    std::optional<std::string> mark_price;
    std::optional<std::string> next_funding_time;
    // The rate's own interval. Annualising without it is wrong by a factor of
    // eight where a venue quotes hourly, so it is absent rather than assumed.
    std::optional<std::int64_t> interval_seconds;
    std::optional<std::string> accrual;
    std::string ts_event;
};

struct ReconBreak {
    std::string venue;
    std::optional<std::string> instrument_id;
    std::string symbol;
    std::string verdict; // break | unknown; `unknown` is NOT agreement
    std::optional<std::string> local_qty;
    std::optional<std::string> venue_qty;
    std::string detail;
    std::string ts_seen;
};

// ------------------------------------------------------------- pages ----

template <typename T> struct Page {
    std::vector<T> items;
    std::optional<std::string> next_cursor; // absent on the last page
};

using OrderPage = Page<Order>;
using FillPage = Page<Fill>;
using PnlPage = Page<PnlIncrement>;
using FundingPage = Page<FundingObservation>;
using ReconBreakPage = Page<ReconBreak>;

// The cursor that continues a page after `last`, derived from the row that was
// emitted rather than from the query that produced it — so a cursor and the
// ORDER BY it resumes cannot drift apart.
[[nodiscard]] Cursor cursor_after(const Order& last);
[[nodiscard]] Cursor cursor_after(const Fill& last);
[[nodiscard]] Cursor cursor_after(const PnlIncrement& last);
[[nodiscard]] Cursor cursor_after(const FundingObservation& last);
[[nodiscard]] Cursor cursor_after(const ReconBreak& last);

// ----------------------------------------------------------- queries ----

// What every listing takes. An empty field is not a filter.
struct PageQuery {
    std::optional<AccountId> account;
    std::optional<InstrumentId> instrument;
    std::optional<UnixNanos> since; // inclusive lower bound on the row's time
    std::optional<Cursor> cursor;
    std::size_t limit{k_default_page};
};

enum class PnlKind : std::uint8_t { Realized, Fee, Funding };

[[nodiscard]] std::string_view pnl_kind_name(PnlKind kind) noexcept;
[[nodiscard]] std::optional<PnlKind> pnl_kind_from_string(std::string_view text) noexcept;

class History {
public:
    explicit History(std::string dsn);
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // False when the database is unreachable. Not fatal: a terminal that can
    // show live state without history is more useful than one that refuses to
    // start because Postgres is down.
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] DatabaseHealth health() const;

    void set_catalogue(ScaleLookup lookup);

    // Newest first. Every read below answers with what it has: a database that
    // went away yields an empty page and reopens on the next call, because the
    // live half of admind must keep answering while it is down.
    [[nodiscard]] OrderPage orders(const PageQuery& query, std::optional<bool> open) const;
    [[nodiscard]] std::optional<Order> order(ClientOrderId id) const;

    // Oldest first, and the truth the order row above is a projection of. Not
    // paged, because the contract gives it no cursor; capped at k_max_page.
    [[nodiscard]] std::vector<OrderEvent> order_events(ClientOrderId id) const;

    [[nodiscard]] FillPage fills(const PageQuery& query) const;

    // Not paged: one row per (strategy, venue, symbol) that has ever traded.
    [[nodiscard]] std::vector<Position> positions(std::optional<AccountId> account,
                                                  std::optional<std::uint16_t> strategy) const;

    [[nodiscard]] PnlPage pnl(const PageQuery& query, std::optional<PnlKind> kind) const;
    [[nodiscard]] FundingPage funding(const PageQuery& query) const;
    [[nodiscard]] ReconBreakPage recon_breaks(const PageQuery& query) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raikiri::admin
