#pragma once

// Pre-trade risk gate. Deliberately a pure function of (cmd, open_count):
// no venue lookups, no clocks, no store reference — the caller supplies the
// one piece of mutable state (how many orders are open) so the gate is
// trivially testable and behaves identically under journal replay. A refusal
// becomes a Denied event; nothing refused here ever reaches the wire.
//
// Structural checks plus position limits. The gate still holds no state: the
// caller assembles an Exposure and passes it in, so replaying a journal through
// the same gate reaches the same verdicts.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace raikiri::oms {

enum class RiskReject : std::uint8_t {
    ReconBreak, // venue and book disagree about this instrument
    TooManyOpen,
    BadQty,
    QtyTooLarge,
    BadPrice,
    BadInstrument,
    BadFlags,
    LongLimit,
    ShortLimit,
    PriceBand,
};


struct RiskConfig {
    std::size_t max_open_orders{64};
    QtyLots max_qty_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
    // Magnitudes, both sides. Default unlimited so a caller that has not
    // decided a limit is not silently given one.
    QtyLots max_long_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
    QtyLots max_short_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};

    // HOW FAR A LIMIT MAY SIT FROM THE MARKET, in basis points.
    //
    // Every other limit here bounds size. This one bounds a typo: a strategy
    // computes its price and will not misplace a decimal, but a person typing
    // 7940 for 79400 places an order that fills instantly against them and is
    // gone before it can be cancelled. Zero disables the check, which is the
    // right default for a caller that has not decided — the same reasoning as
    // the unlimited sizes above.
    std::int64_t max_price_band_bp{0};
};

// The mutable state the gate needs, assembled by the caller.
struct Exposure {
    // Set when reconciliation found this instrument's position in dispute.
    // Only opening commands pass through this gate at all — cancels never
    // consult it, because the escape hatch has to work precisely when
    // something is wrong.
    bool reconciliation_break{false};
    std::size_t open_orders{0};
    QtyLots position_lots{QtyLots::from_raw(0)};     // signed: + long, - short
    QtyLots working_buy_lots{QtyLots::from_raw(0)};  // leaves over live buys
    QtyLots working_sell_lots{QtyLots::from_raw(0)}; // leaves over live sells

    // The market to measure a limit price against. UNDEF IS NOT ZERO: a
    // caller that has no quote leaves this undefined and the band is not
    // checked, because banding against a price nobody published would refuse
    // orders on the strength of a number that does not exist.
    PriceTicks reference_price{};
};

class RiskGate {
public:
    RiskGate() noexcept = default;

    explicit RiskGate(RiskConfig config) noexcept : config_{config} {}

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::expected<void, RiskReject> check(const OrderCmd& cmd,
                                                        const Exposure& exposure) const noexcept {
        // First, and not negotiable by any other field: while the venue and
        // the book disagree about what is held, every quantity the rest of
        // this function reasons about is a number one of the two sides does
        // not recognise.
        if (exposure.reconciliation_break) {
            return std::unexpected{RiskReject::ReconBreak};
        }
        if (!cmd.instrument.valid()) {
            return std::unexpected{RiskReject::BadInstrument};
        }
        if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
            return std::unexpected{RiskReject::BadQty};
        }
        if (cmd.qty > config_.max_qty_lots) {
            return std::unexpected{RiskReject::QtyTooLarge};
        }
        if (cmd.order_type == OrderType::Limit && (cmd.price.is_undef() || cmd.price.raw <= 0)) {
            return std::unexpected{RiskReject::BadPrice};
        }
        // The band, once the price is known to be a price at all.
        if (cmd.order_type == OrderType::Limit && config_.max_price_band_bp > 0 &&
            !exposure.reference_price.is_undef() && exposure.reference_price.raw > 0) {
            const auto reference = exposure.reference_price.raw;
            const auto away = cmd.price.raw > reference ? cmd.price.raw - reference
                                                        : reference - cmd.price.raw;
            // CROSS-MULTIPLIED, never `reference / 10'000 * bp`.
            //
            // These are TICK COUNTS, and dividing first truncates. An
            // instrument quoted at 1.38 with a 0.0001 tick is 13'865 ticks, so
            // `13'865 / 10'000` is 1 and a 500 bp band collapses to 500 ticks —
            // 3.6 %, not 5. The error scales with the tick, so the band was
            // wide on an expensive instrument and arbitrary on a cheap one,
            // which is the opposite of what a fixed basis-point limit means.
            //
            // Both sides stay in int64: `away` never exceeds `reference` in
            // any case worth allowing, and a price of 10^12 ticks times 10'000
            // is still four orders of magnitude below the type's ceiling.
            if (away * 10'000 > reference * config_.max_price_band_bp) {
                return std::unexpected{RiskReject::PriceBand};
            }
        }
        if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
            // Post-only is a maker constraint; a market order is a taker by definition.
            return std::unexpected{RiskReject::BadFlags};
        }
        if (exposure.open_orders >= config_.max_open_orders) {
            return std::unexpected{RiskReject::TooManyOpen};
        }
        // WORST CASE, not current. Every working order on the same side is
        // assumed to fill; opposite-side ones are NOT netted off, because they
        // can be cancelled and a limit that depends on a cancel landing is not
        // a limit. A gate seeing only the position passes sixty-four
        // consecutive buys while the first is still on the wire.
        //
        // ReduceOnly is not exempt. It looks like it should be, but nothing
        // here knows whether the venue enforces the flag, and a limit that
        // trusts an unenforced flag is the one that lets the other side open.
        // Counting it is conservative: it can refuse too much, never too little.
        if (cmd.side == Side::Buy) {
            const auto after = detail::checked_add(
                detail::checked_add(exposure.position_lots.raw, exposure.working_buy_lots.raw),
                cmd.qty.raw);
            if (after > config_.max_long_lots.raw) {
                return std::unexpected{RiskReject::LongLimit};
            }
        } else {
            const auto after = detail::checked_add(
                detail::checked_sub(exposure.working_sell_lots.raw, exposure.position_lots.raw),
                cmd.qty.raw);
            if (after > config_.max_short_lots.raw) {
                return std::unexpected{RiskReject::ShortLimit};
            }
        }
        return {};
    }

private:
    RiskConfig config_{};
};

} // namespace raikiri::oms
