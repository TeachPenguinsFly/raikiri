#pragma once

// pure function of the command and caller-supplied exposure, so replay reaches the same verdicts

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

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

[[nodiscard]] constexpr std::string_view to_string(RiskReject why) noexcept {
    using enum RiskReject;
    switch (why) {
    case ReconBreak:
        return "venue and book disagree on this instrument";
    case TooManyOpen:
        return "too many open orders";
    case BadQty:
        return "quantity is undefined or not positive";
    case QtyTooLarge:
        return "quantity above the per-order ceiling";
    case BadPrice:
        return "limit order with no price, or market order carrying one";
    case BadInstrument:
        return "instrument is undefined";
    case BadFlags:
        return "post-only on a market order";
    case LongLimit:
        return "long exposure limit";
    case ShortLimit:
        return "short exposure limit";
    case PriceBand:
        return "price outside the band around the reference";
    }
    return "unknown";
}

struct RiskConfig {
    std::size_t max_open_orders{64};
    QtyLots max_qty_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
    // magnitudes; unlimited by default
    QtyLots max_long_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
    QtyLots max_short_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};

    // max distance of a limit price from the market, in bp; zero disables
    std::int64_t max_price_band_bp{0};
};

// The mutable state the gate needs, assembled by the caller.
struct Exposure {
    // set when reconciliation disputes this instrument; cancels never consult the gate
    bool reconciliation_break{false};
    std::size_t open_orders{0};
    QtyLots position_lots{QtyLots::from_raw(0)};     // signed: + long, - short
    QtyLots working_buy_lots{QtyLots::from_raw(0)};  // leaves over live buys
    QtyLots working_sell_lots{QtyLots::from_raw(0)}; // leaves over live sells

    // undef skips the band check
    PriceTicks reference_price{};
};

class RiskGate {
public:
    RiskGate() noexcept = default;

    explicit RiskGate(RiskConfig config) noexcept : config_{config} {}

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::expected<void, RiskReject> check(const OrderCmd& cmd, const Exposure& exposure) const noexcept {
        // a reconciliation break refuses before any other check
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
        if (cmd.order_type == OrderType::Limit && config_.max_price_band_bp > 0 &&
            !exposure.reference_price.is_undef() && exposure.reference_price.raw > 0) {
            const auto reference = exposure.reference_price.raw;
            const auto away = cmd.price.raw > reference ? cmd.price.raw - reference : reference - cmd.price.raw;
            // cross-multiplied because tick counts truncate when divided first
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
        // worst case: same-side working orders count as filled, opposite-side ones are not netted,
        // reduce-only included
        if (cmd.side == Side::Buy) {
            const auto after = detail::checked_add(
                detail::checked_add(exposure.position_lots.raw, exposure.working_buy_lots.raw), cmd.qty.raw);
            if (after > config_.max_long_lots.raw) {
                return std::unexpected{RiskReject::LongLimit};
            }
        } else {
            const auto after = detail::checked_add(
                detail::checked_sub(exposure.working_sell_lots.raw, exposure.position_lots.raw), cmd.qty.raw);
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
