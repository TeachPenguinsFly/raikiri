#pragma once

// A position is a fold over its own fills and nothing else writes to it.
//
// It stores what the open quantity COST, not an average entry price. An
// average drifts as partial closes round against it, and — more importantly —
// having nowhere to put one makes it structurally impossible to import the
// venue's. The exchange is authoritative on whether a position exists and
// never on what it cost: it recomputes average entry by its own rules, with
// or without leverage, with fees inside or outside, and two queries can
// disagree. Only our own fill stream can answer that.
//
// See pnl.hpp for the arithmetic and for why leverage appears in none of it.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <compare>
#include <cstdint>

namespace raikiri::portfolio {

// Netting vs hedging is decided HERE and nowhere else. One Position shape, one
// set of formulas; a shared strategy_id nets two strategies together and
// distinct ones keep their books apart. A PositionSide enum would be the same
// choice made twice, in two places that can disagree.
struct PositionKey {
    std::uint16_t strategy_id{0};
    InstrumentId instrument{};

    friend constexpr auto operator<=>(const PositionKey&, const PositionKey&) noexcept = default;
};

class Position {
public:
    // What one fill did, reported back so the caller can journal it without
    // recomputing anything.
    struct Applied {
        // Explicitly zero, NOT default-constructed: a default FixedE9 is the
        // undef sentinel, and "this fill realized nothing" is a real zero that
        // the ledger will write down.
        FixedE9 realized{FixedE9::from_raw(0)};  // booked by the closing part
        FixedE9 close_fee{FixedE9::from_raw(0)}; // the part of `fee` that closed
        FixedE9 open_fee{FixedE9::from_raw(0)};  // the rest; the two sum to `fee` exactly
        bool flipped{false};                     // crossed zero: closed and opened
        bool closed{false};                      // reached flat
    };

    Position() noexcept = default;
    Position(PositionKey key, UnixNanos opened) noexcept : key_{key}, ts_opened_{opened} {}

    // `delta` is the fill quantity the CALLER derived, normally from the drop
    // in leaves_qty rather than from the event's own qty — a venue that resends
    // a fill notification must produce a delta of zero, not a second fill.
    Applied apply_fill(const InstrumentBase& base,
                       Side side,
                       QtyLots delta,
                       PriceTicks price,
                       FixedE9 fee,
                       UnixNanos ts) noexcept;

    [[nodiscard]] PositionKey key() const noexcept { return key_; }
    [[nodiscard]] QtyLots qty() const noexcept { return qty_; } // signed: + long, - short
    [[nodiscard]] bool flat() const noexcept { return qty_.raw == 0; }
    [[nodiscard]] FixedE9 cost() const noexcept { return cost_; } // signed with qty_
    [[nodiscard]] FixedE9 realized() const noexcept { return realized_; } // gross of fees
    [[nodiscard]] FixedE9 fees() const noexcept { return fees_; }
    [[nodiscard]] FixedE9 net_realized() const noexcept;
    [[nodiscard]] std::uint32_t fill_count() const noexcept { return fill_count_; }
    [[nodiscard]] UnixNanos ts_opened() const noexcept { return ts_opened_; }
    [[nodiscard]] UnixNanos ts_updated() const noexcept { return ts_updated_; }

    // Derived for display and for reconciliation break records. Never stored,
    // and never written back from a venue report.
    [[nodiscard]] FixedE9 avg_entry_price(const InstrumentBase& base) const noexcept;

private:
    PositionKey key_{};
    QtyLots qty_{QtyLots::from_raw(0)};
    FixedE9 cost_{FixedE9::from_raw(0)};
    FixedE9 realized_{FixedE9::from_raw(0)};
    FixedE9 fees_{FixedE9::from_raw(0)};
    std::uint32_t fill_count_{0};
    UnixNanos ts_opened_{};
    UnixNanos ts_updated_{};
};

// What closing the whole position at `mark` would realize. A pure function of
// the price argument: there is no stored unrealized field, because such a
// field can only answer "as of when?" and answer it wrongly.
[[nodiscard]] FixedE9 unrealized_pnl(const InstrumentBase& base,
                                     const Position& position,
                                     PriceTicks mark) noexcept;

} // namespace raikiri::portfolio
