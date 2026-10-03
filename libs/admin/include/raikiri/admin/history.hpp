#pragma once

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

// undefined when no raikiri.instruments window holds the row; then no price or size is rendered
struct Scales {
    FixedE9 tick_size{FixedE9::undef()};
    FixedE9 lot_size{FixedE9::undef()};

    [[nodiscard]] std::optional<std::string> price(PriceTicks ticks) const;
    [[nodiscard]] std::optional<std::string> size(QtyLots lots) const;

    // each undefined half taken from `other`
    [[nodiscard]] Scales or_from(const Scales& other) const noexcept;
};

// fallback to the attached gateways' catalogue when no raikiri.instruments window holds the row
using ScaleLookup = std::function<Scales(InstrumentId)>;

// keyset pagination; a page resumes strictly after the last row emitted
struct Cursor {
    std::uint64_t ts{0}; // the ordering timestamp of the last row emitted
    std::string key;     // that row's tiebreak, as text
    std::string key2;    // a second tiebreak, empty where one suffices

    // opaque by contract
    [[nodiscard]] std::string encode() const;
    [[nodiscard]] static std::optional<Cursor> decode(std::string_view text);

    friend bool operator==(const Cursor&, const Cursor&) = default;
};

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
    // the filled amount is the drop in leaves between events, so a resent fill is not booked twice
    std::optional<std::string> leaves;
    std::string status;
    // set only on a rejection; reject_code is the venue's own number
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

struct PnlIncrement {
    std::string source_id; // the idempotency key the ledger dedupes on
    std::string account;
    std::string kind;
    std::optional<std::string> instrument_id;
    std::int64_t strategy_id{0};
    std::string amount; // signed; positive is money gained
    std::string ts_event;
};

// absent fields mean the venue did not publish them
struct FundingObservation {
    std::string instrument_id;
    std::string rate; // a ratio per interval, not a percent and not an APR
    std::optional<std::string> predicted_rate;
    std::optional<std::string> mark_price;
    std::optional<std::string> next_funding_time;
    // the rate's own interval; absent rather than assumed
    std::optional<std::int64_t> interval_seconds;
    std::optional<std::string> accrual;
    std::string ts_event;
};

struct ReconBreak {
    std::string venue;
    std::optional<std::string> instrument_id;
    std::string symbol;
    std::string verdict; // break | unknown; unknown is not agreement
    std::optional<std::string> local_qty;
    std::optional<std::string> venue_qty;
    std::string detail;
    std::string ts_seen;
};

template <typename T> struct Page {
    std::vector<T> items;
    std::optional<std::string> next_cursor; // absent on the last page
};

using OrderPage = Page<Order>;
using FillPage = Page<Fill>;
using PnlPage = Page<PnlIncrement>;
using FundingPage = Page<FundingObservation>;
using ReconBreakPage = Page<ReconBreak>;

// derived from the emitted row so the cursor matches the order by
[[nodiscard]] Cursor cursor_after(const Order& last);
[[nodiscard]] Cursor cursor_after(const Fill& last);
[[nodiscard]] Cursor cursor_after(const PnlIncrement& last);
[[nodiscard]] Cursor cursor_after(const FundingObservation& last);
[[nodiscard]] Cursor cursor_after(const ReconBreak& last);

// an empty field is not a filter
struct PageQuery {
    std::optional<AccountId> account{};
    std::optional<InstrumentId> instrument{};
    std::optional<UnixNanos> since{}; // inclusive lower bound on the row's time
    std::optional<Cursor> cursor{};
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

    // false when the database is unreachable; not fatal
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] DatabaseHealth health() const;

    void set_catalogue(ScaleLookup lookup);

    // newest first; an unreachable database yields an empty page and reconnects on the next call
    [[nodiscard]] OrderPage orders(const PageQuery& query, std::optional<bool> open) const;
    [[nodiscard]] std::optional<Order> order(ClientOrderId id) const;

    // oldest first; not paged, capped at k_max_page
    [[nodiscard]] std::vector<OrderEvent> order_events(ClientOrderId id) const;

    [[nodiscard]] FillPage fills(const PageQuery& query) const;

    // not paged: one row per (strategy, venue, symbol) that has traded
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
