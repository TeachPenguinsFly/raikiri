#pragma once

// a fold over its own fills; stores cost, never an average entry from the venue

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/portfolio/pnl.hpp>

#include <compare>
#include <cstdint>

namespace raikiri::portfolio {

// netting or hedging is decided by strategy_id here and nowhere else
struct PositionKey {
    std::uint16_t strategy_id{0};
    InstrumentId instrument{};

    friend constexpr auto operator<=>(const PositionKey&, const PositionKey&) noexcept = default;
};

class Position {
public:
    struct Applied {
        FixedE9 realized{FixedE9::from_raw(0)};  // booked by the closing part
        FixedE9 close_fee{FixedE9::from_raw(0)}; // the part of `fee` that closed
        FixedE9 open_fee{FixedE9::from_raw(0)};  // the rest; the two sum to `fee` exactly
        bool flipped{false};                     // crossed zero: closed and opened
        bool closed{false};                      // reached flat
    };

    Position() noexcept = default;

    Position(PositionKey key, UnixNanos opened) noexcept : key_{key}, ts_opened_{opened} {}

    // for journal replay only
    struct Restored {
        QtyLots qty{QtyLots::from_raw(0)};
        FixedE9 cost{FixedE9::from_raw(0)};
        FixedE9 realized{FixedE9::from_raw(0)};
        FixedE9 fees{FixedE9::from_raw(0)};
        std::uint32_t fill_count{0};
        UnixNanos ts_opened{};
        UnixNanos ts_updated{};
    };

    Position(PositionKey key, const Restored& state) noexcept
        : key_{key}, qty_{state.qty}, cost_{state.cost}, realized_{state.realized},
          fees_{state.fees}, fill_count_{state.fill_count}, ts_opened_{state.ts_opened},
          ts_updated_{state.ts_updated} {}

    // `delta` is the caller's fill quantity, normally the drop in leaves
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

    // derived; never stored or taken from a venue report
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

// what closing the whole position at `mark` would realize
[[nodiscard]] FixedE9
unrealized_pnl(const InstrumentBase& base, const Position& position, PriceTicks mark) noexcept;

} // namespace raikiri::portfolio
