#pragma once

// pure function of the command and caller-supplied exposure, so replay reaches the same verdicts

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/oms/notional.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
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
    NoPrice,
    Unpriceable,
    UnpricedExposure,
    OrderNotional,
    InstrumentNotional,
    StrategyNotional,
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
        return "lot ceiling for this instrument kind";
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
    case NoPrice:
        return "market order with no touch to value it at";
    case Unpriceable:
        return "instrument cannot be valued in its quote currency";
    case UnpricedExposure:
        return "the strategy holds exposure that has no quote to value it";
    case OrderNotional:
        return "per-order notional limit";
    case InstrumentNotional:
        return "open notional limit per instrument";
    case StrategyNotional:
        return "open notional limit per strategy";
    }
    return "unknown";
}

// a fat-finger backstop only, since a lot means a different amount on every instrument; undef is off
struct LotCeilings {
    QtyLots spot{};
    QtyLots perp{};
    QtyLots future{};
    QtyLots option{};

    [[nodiscard]] constexpr QtyLots of(MarketType market) const noexcept {
        switch (market) {
        case MarketType::Spot:
            return spot;
        case MarketType::Perp:
            return perp;
        case MarketType::Future:
            return future;
        case MarketType::Option:
            return option;
        }
        return QtyLots::undef();
    }
};

struct RiskConfig {
    std::size_t max_open_orders{64};
    LotCeilings max_lots{};
    // magnitudes; unlimited by default
    QtyLots max_long_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};
    QtyLots max_short_lots{QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())};

    // max distance of a limit price from the market, in bp; zero disables
    std::int64_t max_price_band_bp{0};

    // quote currency, e9; undef is off
    FixedE9 max_order_notional{};
    FixedE9 max_instrument_notional{};
    FixedE9 max_strategy_notional{};

    [[nodiscard]] constexpr bool prices_orders() const noexcept {
        return !max_order_notional.is_undef() || !max_instrument_notional.is_undef() ||
               !max_strategy_notional.is_undef();
    }
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

    // read only when a notional limit is set
    const Instrument* instrument{nullptr};
    PriceTicks bid{};
    PriceTicks ask{};
    // the strategy's open notional on its other instruments, e9 quote; undef when one has no quote
    FixedE9 strategy_open_notional{FixedE9::from_raw(0)};
};

struct RiskRefusal {
    RiskReject why{RiskReject::BadQty};
    // lots for the lot ceiling, e9 quote for the notional limits; zero where no limit applies
    std::int64_t limit{0};
    std::int64_t value{0};
    // undef until the order is valued
    FixedE9 order_notional{};
};

// lots on the worse side, as for the lot limits: same-side working orders count as filled
[[nodiscard]] inline std::int64_t open_lots(QtyLots position, QtyLots working_buy, QtyLots working_sell) noexcept {
    const auto longest = detail::checked_add(position.raw, working_buy.raw);
    const auto shortest = detail::checked_sub(working_sell.raw, position.raw);
    return std::max<std::int64_t>({0, longest, shortest});
}

// names the limit, the value it would reach and the order's notional
[[nodiscard]] std::string describe(const RiskRefusal& refusal);

class RiskGate {
public:
    RiskGate() noexcept = default;

    explicit RiskGate(RiskConfig config) noexcept : config_{config} {}

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::expected<void, RiskRefusal> check(const OrderCmd& cmd, const Exposure& exposure) const noexcept {
        // a reconciliation break refuses before any other check
        if (exposure.reconciliation_break) {
            return std::unexpected{RiskRefusal{.why = RiskReject::ReconBreak}};
        }
        if (!cmd.instrument.valid()) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BadInstrument}};
        }
        if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BadQty}};
        }
        if (const auto ceiling = config_.max_lots.of(cmd.instrument.market());
            !ceiling.is_undef() && cmd.qty > ceiling) {
            return std::unexpected{
                RiskRefusal{.why = RiskReject::QtyTooLarge, .limit = ceiling.raw, .value = cmd.qty.raw}};
        }
        if (cmd.order_type == OrderType::Limit && (cmd.price.is_undef() || cmd.price.raw <= 0)) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BadPrice}};
        }
        if (cmd.order_type == OrderType::Limit && config_.max_price_band_bp > 0 &&
            !exposure.reference_price.is_undef() && exposure.reference_price.raw > 0) {
            const auto reference = exposure.reference_price.raw;
            const auto away = cmd.price.raw > reference ? cmd.price.raw - reference : reference - cmd.price.raw;
            // cross-multiplied because tick counts truncate when divided first
            if (away * 10'000 > reference * config_.max_price_band_bp) {
                return std::unexpected{RiskRefusal{.why = RiskReject::PriceBand}};
            }
        }
        if (cmd.order_type == OrderType::Market && has_flag(cmd.flags, OrderFlags::PostOnly)) {
            // Post-only is a maker constraint; a market order is a taker by definition.
            return std::unexpected{RiskRefusal{.why = RiskReject::BadFlags}};
        }
        if (exposure.open_orders >= config_.max_open_orders) {
            return std::unexpected{RiskRefusal{.why = RiskReject::TooManyOpen}};
        }
        // worst case: same-side working orders count as filled, opposite-side ones are not netted,
        // reduce-only included
        if (cmd.side == Side::Buy) {
            const auto after = detail::checked_add(
                detail::checked_add(exposure.position_lots.raw, exposure.working_buy_lots.raw), cmd.qty.raw);
            if (after > config_.max_long_lots.raw) {
                return std::unexpected{
                    RiskRefusal{.why = RiskReject::LongLimit, .limit = config_.max_long_lots.raw, .value = after}};
            }
        } else {
            const auto after = detail::checked_add(
                detail::checked_sub(exposure.working_sell_lots.raw, exposure.position_lots.raw), cmd.qty.raw);
            if (after > config_.max_short_lots.raw) {
                return std::unexpected{
                    RiskRefusal{.why = RiskReject::ShortLimit, .limit = config_.max_short_lots.raw, .value = after}};
            }
        }
        if (config_.prices_orders()) {
            return check_notional(cmd, exposure);
        }
        return {};
    }

private:
    [[nodiscard]] std::expected<void, RiskRefusal> check_notional(const OrderCmd& cmd,
                                                                  const Exposure& exposure) const noexcept {
        if (exposure.instrument == nullptr) {
            return std::unexpected{RiskRefusal{.why = RiskReject::Unpriceable}};
        }
        const bool buy = cmd.side == Side::Buy;
        const auto at = cmd.order_type == OrderType::Limit ? cmd.price : buy ? exposure.ask : exposure.bid;
        const auto order = notional(*exposure.instrument, at, cmd.qty);
        if (!order.has_value()) {
            return std::unexpected{RiskRefusal{
                .why = order.error() == NotionalError::NoPrice ? RiskReject::NoPrice : RiskReject::Unpriceable}};
        }
        const auto order_e9 = to_e9(*order);
        if (!config_.max_order_notional.is_undef() && order_e9 > config_.max_order_notional) {
            return std::unexpected{RiskRefusal{.why = RiskReject::OrderNotional,
                                               .limit = config_.max_order_notional.raw,
                                               .value = order_e9.raw,
                                               .order_notional = order_e9}};
        }
        // an order that does not grow the worst case is never held back, so a reduction always passes
        const auto before = open_lots(exposure.position_lots, exposure.working_buy_lots, exposure.working_sell_lots);
        const auto after = open_lots(exposure.position_lots,
                                     buy ? exposure.working_buy_lots + cmd.qty : exposure.working_buy_lots,
                                     buy ? exposure.working_sell_lots : exposure.working_sell_lots + cmd.qty);
        if (after <= before) {
            return {};
        }
        // the instrument's whole worst case is valued at this order's price
        const auto held_e9 = to_e9(notional(*exposure.instrument, at, QtyLots::from_raw(after)).value_or(*order));
        if (!config_.max_instrument_notional.is_undef() && held_e9 > config_.max_instrument_notional) {
            return std::unexpected{RiskRefusal{.why = RiskReject::InstrumentNotional,
                                               .limit = config_.max_instrument_notional.raw,
                                               .value = held_e9.raw,
                                               .order_notional = order_e9}};
        }
        if (config_.max_strategy_notional.is_undef()) {
            return {};
        }
        if (exposure.strategy_open_notional.is_undef()) {
            return std::unexpected{RiskRefusal{.why = RiskReject::UnpricedExposure, .order_notional = order_e9}};
        }
        const auto total = to_e9(Notional128{static_cast<int128_t>(exposure.strategy_open_notional.raw) + held_e9.raw});
        if (total > config_.max_strategy_notional) {
            return std::unexpected{RiskRefusal{.why = RiskReject::StrategyNotional,
                                               .limit = config_.max_strategy_notional.raw,
                                               .value = total.raw,
                                               .order_notional = order_e9}};
        }
        return {};
    }

    RiskConfig config_{};
};

} // namespace raikiri::oms
