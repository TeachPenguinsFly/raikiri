#pragma once

// pure function of the command and caller-supplied exposure, so replay reaches the same verdicts

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/oms/book_state.hpp>
#include <raikiri/oms/notional.hpp>
#include <raikiri/oms/short_option.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

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
    NoUnderlying,
    OptionPremium,
    OptionLots,
    NoCapital,
    BookHalted,
    BookReducing,
    OrderRate,
    RepeatedExecution,
    StaleQuote,
    NakedShort,
    ShortMarginUnknown,
    ShortMargin,
    ShortGreeks,
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
    case NoUnderlying:
        return "coin-premium option with no underlying price to value it";
    case OptionPremium:
        return "per-order option premium limit";
    case OptionLots:
        return "open option contracts limit per instrument";
    case NoCapital:
        return "no desk capital has been set";
    case BookHalted:
        return "the book is halted: only cancels pass";
    case BookReducing:
        return "the book is reducing: only orders that shrink the position pass";
    case OrderRate:
        return "order rate limit for this book";
    case RepeatedExecution:
        return "the book keeps filling and re-entering";
    case StaleQuote:
        return "the quote behind this order is too old";
    case NakedShort:
        return "naked short option exposure limit";
    case ShortMarginUnknown:
        return "opening a short option with no margin figure to check";
    case ShortMargin:
        return "estimated short option margin exceeds what the venue has available";
    case ShortGreeks:
        return "the book is past its short vega or gamma limit, so it may not sell more options";
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

    // options only: premium in quote per order, e9, and worst-case open lots per instrument; undef is off
    FixedE9 max_option_premium{};
    QtyLots max_option_open_lots{};

    // orders sent per window, and fill-then-re-entry cycles per window; zero is off
    std::uint32_t max_orders_in_window{0};
    std::uint64_t order_window_ns{1'000'000'000};
    std::uint32_t max_repeat_entries{0};
    std::uint64_t repeat_window_ns{60'000'000'000};
    // zero is off; on, an instrument with no quote at all counts as too old
    std::uint64_t max_quote_age_ns{0};
    // an identical order inside this is a duplicate; zero is a book that repeats on purpose
    std::uint64_t duplicate_window_ns{1'000'000'000ULL};
    // worst-case usd across the book's short options, e9; undef is off
    FixedE9 max_naked_short{};
    // magnitudes of the book's net vega and gamma, read by the periodic check, e9 usd; undef is off
    FixedE9 max_short_vega{};
    FixedE9 max_short_gamma{};
    // an estimate is not the venue's own figure, so a short opening must leave this much spare
    std::int64_t short_margin_buffer_bp{500};

    [[nodiscard]] constexpr bool prices_orders() const noexcept {
        return !max_order_notional.is_undef() || !max_instrument_notional.is_undef() ||
               !max_strategy_notional.is_undef();
    }
};

// read only for an option sell that would open or enlarge a short
struct ShortOption {
    ShortMarginRates rates{};
    FixedE9 index{};
    FixedE9 forward{};
    FixedE9 mark{};
    // in the option's settlement asset, as the venue last said; undef refuses
    FixedE9 margin_available{};
    // what this book's working short openings will take once filled, same asset
    FixedE9 margin_working{FixedE9::from_raw(0)};
    // the book's other instruments, e9 usd; undef when one cannot be valued
    FixedE9 naked_besides{FixedE9::from_raw(0)};
    bool greeks_breached{false};
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
    // quote per base unit, e9; values a coin-premium option, undef elsewhere
    FixedE9 underlying_price{};
    // the strategy's open notional on its other instruments, e9 quote; undef when one has no quote
    FixedE9 strategy_open_notional{FixedE9::from_raw(0)};

    BookState book_state{BookState::Active};
    bool capital_set{true};
    // counted by the caller up to cmd.ts_created, so a replay of the same stream counts the same
    std::uint32_t orders_in_window{0};
    std::uint32_t repeats_in_window{0};
    // the book filled on this instrument since its last entry there
    bool filled_since_entry{false};
    // this box's receive stamp of the quote behind bid and ask; zero when there is none
    UnixNanos quote_ts{};
    ShortOption short_option{};
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

// same-side working orders count as filled, so an order that passes cannot flip the position however they end
[[nodiscard]] inline bool shrinks_position(const OrderCmd& cmd, const Exposure& exposure) noexcept {
    const auto held = exposure.position_lots.raw;
    if (cmd.side == Side::Buy) {
        return held < 0 && detail::checked_add(exposure.working_buy_lots.raw, cmd.qty.raw) <= -held;
    }
    return held > 0 && detail::checked_add(exposure.working_sell_lots.raw, cmd.qty.raw) <= held;
}

[[nodiscard]] inline bool grows_worst_case(const OrderCmd& cmd, const Exposure& exposure) noexcept {
    const bool buy = cmd.side == Side::Buy;
    const auto before = open_lots(exposure.position_lots, exposure.working_buy_lots, exposure.working_sell_lots);
    const auto after = open_lots(exposure.position_lots,
                                 buy ? exposure.working_buy_lots + cmd.qty : exposure.working_buy_lots,
                                 buy ? exposure.working_sell_lots : exposure.working_sell_lots + cmd.qty);
    return after > before;
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
        if (!exposure.capital_set) {
            return std::unexpected{RiskRefusal{.why = RiskReject::NoCapital}};
        }
        if (exposure.book_state == BookState::Halted) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BookHalted}};
        }
        if (!cmd.instrument.valid()) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BadInstrument}};
        }
        if (cmd.qty.is_undef() || cmd.qty.raw <= 0) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BadQty}};
        }
        if (exposure.book_state == BookState::Reducing && !shrinks_position(cmd, exposure)) {
            return std::unexpected{RiskRefusal{.why = RiskReject::BookReducing}};
        }
        if (auto refused = check_pace(cmd, exposure); !refused.has_value()) {
            return refused;
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
        if (cmd.instrument.market() == MarketType::Option) {
            if (auto refused = check_option(cmd, exposure); !refused.has_value()) {
                return refused;
            }
            if (auto refused = check_short_option(cmd, exposure); !refused.has_value()) {
                return refused;
            }
        }
        if (config_.prices_orders()) {
            return check_notional(cmd, exposure);
        }
        return {};
    }

private:
    [[nodiscard]] std::expected<void, RiskRefusal> check_pace(const OrderCmd& cmd,
                                                              const Exposure& exposure) const noexcept {
        if (config_.max_orders_in_window > 0 && exposure.orders_in_window >= config_.max_orders_in_window) {
            return std::unexpected{RiskRefusal{.why = RiskReject::OrderRate,
                                               .limit = config_.max_orders_in_window,
                                               .value = std::int64_t{exposure.orders_in_window} + 1}};
        }
        if (config_.max_repeat_entries > 0 && exposure.filled_since_entry &&
            exposure.repeats_in_window >= config_.max_repeat_entries && grows_worst_case(cmd, exposure)) {
            return std::unexpected{RiskRefusal{.why = RiskReject::RepeatedExecution,
                                               .limit = config_.max_repeat_entries,
                                               .value = std::int64_t{exposure.repeats_in_window} + 1}};
        }
        // a quiet market must not trap a position, so closing leans on no quote
        if (config_.max_quote_age_ns > 0 && !shrinks_position(cmd, exposure)) {
            if (exposure.quote_ts.ns == 0) {
                return std::unexpected{RiskRefusal{.why = RiskReject::StaleQuote}};
            }
            const auto age = cmd.ts_created.ns > exposure.quote_ts.ns ? cmd.ts_created.ns - exposure.quote_ts.ns : 0;
            if (age > config_.max_quote_age_ns) {
                return std::unexpected{RiskRefusal{.why = RiskReject::StaleQuote,
                                                   .limit = static_cast<std::int64_t>(config_.max_quote_age_ns),
                                                   .value = static_cast<std::int64_t>(age)}};
            }
        }
        return {};
    }

    [[nodiscard]] static RiskReject reject_of(NotionalError why) noexcept {
        switch (why) {
        case NotionalError::NoPrice:
            return RiskReject::NoPrice;
        case NotionalError::NoUnderlying:
            return RiskReject::NoUnderlying;
        case NotionalError::Unpriceable:
            break;
        }
        return RiskReject::Unpriceable;
    }

    [[nodiscard]] std::expected<void, RiskRefusal> check_option(const OrderCmd& cmd,
                                                                const Exposure& exposure) const noexcept {
        if (!grows_worst_case(cmd, exposure)) {
            return {};
        }
        const bool buy = cmd.side == Side::Buy;
        const auto after = open_lots(exposure.position_lots,
                                     buy ? exposure.working_buy_lots + cmd.qty : exposure.working_buy_lots,
                                     buy ? exposure.working_sell_lots : exposure.working_sell_lots + cmd.qty);
        if (!config_.max_option_open_lots.is_undef() && after > config_.max_option_open_lots.raw) {
            return std::unexpected{
                RiskRefusal{.why = RiskReject::OptionLots, .limit = config_.max_option_open_lots.raw, .value = after}};
        }
        if (config_.max_option_premium.is_undef()) {
            return {};
        }
        if (exposure.instrument == nullptr) {
            return std::unexpected{RiskRefusal{.why = RiskReject::Unpriceable}};
        }
        const auto at = cmd.order_type == OrderType::Limit ? cmd.price : buy ? exposure.ask : exposure.bid;
        const auto premium = notional(*exposure.instrument, at, cmd.qty, exposure.underlying_price);
        if (!premium.has_value()) {
            return std::unexpected{RiskRefusal{.why = reject_of(premium.error())}};
        }
        if (const auto e9 = to_e9(*premium); e9 > config_.max_option_premium) {
            return std::unexpected{RiskRefusal{.why = RiskReject::OptionPremium,
                                               .limit = config_.max_option_premium.raw,
                                               .value = e9.raw,
                                               .order_notional = e9}};
        }
        return {};
    }

    // a buy, or a sell that only takes down a long, never reaches a limit here
    [[nodiscard]] std::expected<void, RiskRefusal> check_short_option(const OrderCmd& cmd,
                                                                      const Exposure& exposure) const noexcept {
        if (cmd.side != Side::Sell) {
            return {};
        }
        const auto before = std::max<std::int64_t>(
            0, detail::checked_sub(exposure.working_sell_lots.raw, exposure.position_lots.raw));
        const auto after = detail::checked_sub(detail::checked_add(exposure.working_sell_lots.raw, cmd.qty.raw),
                                               exposure.position_lots.raw);
        if (after <= before) {
            return {};
        }
        const auto& inputs = exposure.short_option;
        if (inputs.greeks_breached) {
            return std::unexpected{RiskRefusal{.why = RiskReject::ShortGreeks}};
        }
        const auto* option = exposure.instrument != nullptr ? std::get_if<OptionInstrument>(exposure.instrument)
                                                            : nullptr;
        if (option == nullptr) {
            return std::unexpected{RiskRefusal{.why = RiskReject::Unpriceable}};
        }
        if (!config_.max_naked_short.is_undef()) {
            const auto mine = short_worst_case(
                *option, QtyLots::from_raw(after), inputs.index.is_undef() ? inputs.forward : inputs.index);
            if (mine.is_undef()) {
                return std::unexpected{RiskRefusal{.why = RiskReject::NoUnderlying}};
            }
            if (inputs.naked_besides.is_undef()) {
                return std::unexpected{RiskRefusal{.why = RiskReject::UnpricedExposure}};
            }
            const auto total = to_e9(Notional128{static_cast<int128_t>(inputs.naked_besides.raw) + mine.raw});
            if (total > config_.max_naked_short) {
                return std::unexpected{RiskRefusal{
                    .why = RiskReject::NakedShort, .limit = config_.max_naked_short.raw, .value = total.raw}};
            }
        }
        return check_short_margin(cmd, exposure, *option, QtyLots::from_raw(after - before));
    }

    [[nodiscard]] std::expected<void, RiskRefusal> check_short_margin(const OrderCmd& cmd,
                                                                      const Exposure& exposure,
                                                                      const OptionInstrument& option,
                                                                      QtyLots opening) const noexcept {
        const auto& inputs = exposure.short_option;
        const auto at = cmd.order_type == OrderType::Limit ? cmd.price : exposure.bid;
        const auto margin = short_margin(
            inputs.rates,
            {.premium = option.premium,
             .put_call = option.put_call,
             .strike = option.base.ticks_to_price(option.strike),
             .index = inputs.index.is_undef() ? inputs.forward : inputs.index,
             .forward = inputs.forward.is_undef() ? inputs.index : inputs.forward,
             .mark = inputs.mark,
             .order_price = at.is_undef() ? FixedE9::undef() : option.base.ticks_to_price(at)});
        const auto units = option_units(option.base, opening);
        if (!margin.has_value() || units.is_undef() || inputs.margin_available.is_undef() ||
            inputs.margin_working.is_undef()) {
            return std::unexpected{RiskRefusal{.why = RiskReject::ShortMarginUnknown}};
        }
        const auto buffered = static_cast<int128_t>(margin->order.raw) * units.raw / k_e9 *
                              (10'000 + config_.short_margin_buffer_bp) / 10'000;
        const auto required = to_e9(Notional128{buffered + inputs.margin_working.raw});
        if (required.raw > inputs.margin_available.raw) {
            return std::unexpected{RiskRefusal{
                .why = RiskReject::ShortMargin, .limit = inputs.margin_available.raw, .value = required.raw}};
        }
        return {};
    }

    [[nodiscard]] std::expected<void, RiskRefusal> check_notional(const OrderCmd& cmd,
                                                                  const Exposure& exposure) const noexcept {
        if (exposure.instrument == nullptr) {
            return std::unexpected{RiskRefusal{.why = RiskReject::Unpriceable}};
        }
        const bool buy = cmd.side == Side::Buy;
        const auto at = cmd.order_type == OrderType::Limit ? cmd.price : buy ? exposure.ask : exposure.bid;
        const auto order = notional(*exposure.instrument, at, cmd.qty, exposure.underlying_price);
        if (!order.has_value()) {
            return std::unexpected{RiskRefusal{.why = reject_of(order.error())}};
        }
        // an order that does not grow the worst case is never held back, so a reduction always passes, even
        // one whose exit price puts it past the per-order cap the entry fitted under
        const auto before = open_lots(exposure.position_lots, exposure.working_buy_lots, exposure.working_sell_lots);
        const auto after = open_lots(exposure.position_lots,
                                     buy ? exposure.working_buy_lots + cmd.qty : exposure.working_buy_lots,
                                     buy ? exposure.working_sell_lots : exposure.working_sell_lots + cmd.qty);
        if (after <= before) {
            return {};
        }
        const auto order_e9 = to_e9(*order);
        if (!config_.max_order_notional.is_undef() && order_e9 > config_.max_order_notional) {
            return std::unexpected{RiskRefusal{.why = RiskReject::OrderNotional,
                                               .limit = config_.max_order_notional.raw,
                                               .value = order_e9.raw,
                                               .order_notional = order_e9}};
        }
        // the instrument's whole worst case is valued at this order's price
        const auto held_e9 = to_e9(
            notional(*exposure.instrument, at, QtyLots::from_raw(after), exposure.underlying_price).value_or(*order));
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
