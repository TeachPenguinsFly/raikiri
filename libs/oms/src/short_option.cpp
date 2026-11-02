#include <raikiri/oms/short_option.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace raikiri::oms {
namespace internal {

[[nodiscard]] constexpr FixedE9 rate(std::int64_t per_mille_tenths) noexcept {
    return FixedE9::from_raw(per_mille_tenths * 1'000'000);
}

[[nodiscard]] bool usable(FixedE9 value) noexcept {
    return !value.is_undef() && value.raw >= 0;
}

[[nodiscard]] FixedE9 narrow(int128_t value) noexcept {
    if (value > std::numeric_limits<std::int64_t>::max() || value <= std::numeric_limits<std::int64_t>::min()) {
        return FixedE9::undef();
    }
    return FixedE9::from_raw(static_cast<std::int64_t>(value));
}

[[nodiscard]] FixedE9 mul(FixedE9 a, FixedE9 b) noexcept {
    if (a.is_undef() || b.is_undef()) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) * b.raw / k_e9);
}

[[nodiscard]] FixedE9 div(FixedE9 a, FixedE9 b) noexcept {
    if (a.is_undef() || b.is_undef() || b.raw == 0) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) * k_e9 / b.raw);
}

[[nodiscard]] FixedE9 add(FixedE9 a, FixedE9 b) noexcept {
    if (a.is_undef() || b.is_undef()) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) + b.raw);
}

[[nodiscard]] FixedE9 sub(FixedE9 a, FixedE9 b) noexcept {
    if (a.is_undef() || b.is_undef()) {
        return FixedE9::undef();
    }
    return narrow(static_cast<int128_t>(a.raw) - b.raw);
}

// undef wins, so a missing input never reads as the other side of the comparison
[[nodiscard]] FixedE9 most(FixedE9 a, FixedE9 b) noexcept {
    return a.is_undef() || b.is_undef() ? FixedE9::undef() : FixedE9::from_raw(std::max(a.raw, b.raw));
}

[[nodiscard]] FixedE9 least(FixedE9 a, FixedE9 b) noexcept {
    return a.is_undef() || b.is_undef() ? FixedE9::undef() : FixedE9::from_raw(std::min(a.raw, b.raw));
}

[[nodiscard]] FixedE9 otm_amount(const ShortMarginInput& in, FixedE9 underlying) noexcept {
    const auto away = in.put_call == PutCall::Call ? sub(in.strike, underlying) : sub(underlying, in.strike);
    return most(away, FixedE9::from_raw(0));
}

[[nodiscard]] ShortMargin deribit(const ShortMarginRates& r, const ShortMarginInput& in) noexcept {
    const bool call = in.put_call == PutCall::Call;
    ShortMargin out{};
    if (in.premium == OptionUnit::Base) {
        const auto fraction = sub(r.im_base, div(otm_amount(in, in.index), in.index));
        out.initial = add(most(fraction, r.im_floor), in.mark);
        out.maintenance = add(call ? r.mm : most(r.mm, mul(r.mm, in.mark)), in.mark);
    } else {
        const auto fraction = sub(r.im_base, div(otm_amount(in, in.forward), in.forward));
        const auto risk = call ? mul(most(fraction, r.im_floor), in.index)
                               : most(mul(fraction, in.index), mul(r.im_floor, in.strike));
        out.initial = add(risk, in.mark);
        out.maintenance = add(mul(r.mm, call ? in.index : least(in.index, in.strike)), in.mark);
    }
    // nothing readable says a sell order is credited its premium, so it reserves the position's figure
    out.order = out.initial;
    return out;
}

[[nodiscard]] ShortMargin okx(const ShortMarginRates& r, const ShortMarginInput& in) noexcept {
    constexpr auto k_min_open = rate(100);
    const bool call = in.put_call == PutCall::Call;
    const auto fraction = sub(r.im_base, div(otm_amount(in, in.forward), in.forward));
    const auto risk = mul(most(r.im_floor, fraction), r.factor);
    ShortMargin out{};
    if (in.premium == OptionUnit::Base) {
        out.initial = add(risk, in.mark);
        out.maintenance = add(mul(call ? r.mm : most(r.mm, mul(r.mm, in.mark)), r.factor), in.mark);
        out.order = most(sub(out.initial, in.order_price), k_min_open);
    } else {
        out.initial = add(mul(risk, in.index), in.mark);
        const auto mm = call ? mul(r.mm, in.index) : most(mul(r.mm, in.index), mul(r.mm, in.mark));
        out.maintenance = add(mul(mm, r.factor), in.mark);
        out.order = most(sub(out.initial, in.order_price), mul(k_min_open, in.index));
    }
    return out;
}

[[nodiscard]] ShortMargin bybit(const ShortMarginRates& r, const ShortMarginInput& in) noexcept {
    const auto risk = most(sub(mul(r.im_base, in.index), otm_amount(in, in.index)), mul(r.im_floor, in.index));
    ShortMargin out{};
    out.maintenance = add(add(most(mul(r.mm, in.index), mul(r.mm, in.mark)), in.mark),
                          mul(r.liquidation_fee, in.index));
    out.initial = most(add(risk, in.mark), out.maintenance);
    const auto opening = most(add(risk, most(in.order_price, in.mark)), out.maintenance);
    out.order = most(sub(opening, in.order_price), FixedE9::from_raw(0));
    return out;
}

[[nodiscard]] ShortMargin gate(const ShortMarginRates& r, const ShortMarginInput& in) noexcept {
    const bool call = in.put_call == PutCall::Call;
    const auto low = mul(r.im_floor, call ? in.index : add(in.index, in.mark));
    const auto risk = most(low, sub(mul(r.im_base, in.index), otm_amount(in, in.index)));
    ShortMargin out{};
    out.initial = add(risk, in.mark);
    out.maintenance = add(call ? mul(r.mm, in.index) : most(mul(r.mm, in.index), mul(r.mm, in.mark)), in.mark);
    out.order = add(risk, least(in.mark, in.order_price));
    return out;
}

} // namespace internal

ShortMarginRates short_margin_rates(VenueId venue, std::string_view underlying) noexcept {
    using internal::rate;
    const bool btc = underlying == "BTC";
    const bool eth = underlying == "ETH";
    switch (venue) {
    case VenueId::Deribit:
        if (btc || eth) {
            return {.formula = MarginFormula::Deribit, .im_floor = rate(100), .im_base = rate(150), .mm = rate(75)};
        }
        // the page's second set, which it does not assign to products; the larger one
        return {.formula = MarginFormula::Deribit, .im_floor = rate(130), .im_base = rate(200), .mm = rate(100)};
    case VenueId::Okx:
        if (btc || eth) {
            return {.formula = MarginFormula::Okx,
                    .im_floor = rate(100),
                    .im_base = rate(150),
                    .mm = btc ? rate(30) : rate(50)};
        }
        return {};
    case VenueId::Bybit: {
        const auto of = [](std::int64_t mm, std::int64_t base, std::int64_t floor) {
            return ShortMarginRates{.formula = MarginFormula::Bybit,
                                    .im_floor = rate(floor),
                                    .im_base = rate(base),
                                    .mm = rate(mm),
                                    .liquidation_fee = rate(2)};
        };
        if (btc) {
            return of(30, 100, 50);
        }
        if (eth) {
            return of(50, 100, 50);
        }
        if (underlying == "SOL") {
            return of(30, 150, 100);
        }
        if (underlying == "XRP" || underlying == "MNT" || underlying == "DOGE") {
            return of(100, 200, 130);
        }
        return {};
    }
    case VenueId::Gate:
        if (btc || eth) {
            return {.formula = MarginFormula::Gate, .im_floor = rate(100), .im_base = rate(150), .mm = rate(75)};
        }
        if (underlying == "DOGE" || underlying == "LTC" || underlying == "SOL") {
            return {.formula = MarginFormula::Gate, .im_floor = rate(150), .im_base = rate(200), .mm = rate(100)};
        }
        return {};
    default:
        return {};
    }
}

std::expected<ShortMargin, ShortMarginError> short_margin(const ShortMarginRates& rates,
                                                          const ShortMarginInput& input) noexcept {
    if (rates.formula == MarginFormula::Unpublished) {
        return std::unexpected{ShortMarginError::NoFormula};
    }
    if (input.premium != OptionUnit::Base && input.premium != OptionUnit::Quote) {
        return std::unexpected{ShortMarginError::WrongUnit};
    }
    const bool quote_only = rates.formula == MarginFormula::Bybit || rates.formula == MarginFormula::Gate;
    if (quote_only && input.premium != OptionUnit::Quote) {
        return std::unexpected{ShortMarginError::WrongUnit};
    }
    if (!internal::usable(input.strike) || !internal::usable(input.index) || !internal::usable(input.forward) ||
        !internal::usable(input.mark) || !internal::usable(input.order_price) || input.index.raw == 0 ||
        input.forward.raw == 0) {
        return std::unexpected{ShortMarginError::NoInput};
    }
    ShortMargin out{};
    switch (rates.formula) {
    case MarginFormula::Deribit:
        out = internal::deribit(rates, input);
        break;
    case MarginFormula::Okx:
        out = internal::okx(rates, input);
        break;
    case MarginFormula::Bybit:
        out = internal::bybit(rates, input);
        break;
    case MarginFormula::Gate:
        out = internal::gate(rates, input);
        break;
    case MarginFormula::Unpublished:
        break;
    }
    if (out.initial.is_undef() || out.maintenance.is_undef() || out.order.is_undef()) {
        return std::unexpected{ShortMarginError::NoInput};
    }
    return out;
}

FixedE9 option_units(const InstrumentBase& base, QtyLots lots) noexcept {
    if (lots.is_undef() || base.lot_size.is_undef() || base.lot_size.raw <= 0 || base.contract_value.is_undef() ||
        base.contract_value.raw <= 0) {
        return FixedE9::undef();
    }
    const int128_t magnitude = lots.raw < 0 ? -static_cast<int128_t>(lots.raw) : lots.raw;
    int128_t units{};
    if (__builtin_mul_overflow(magnitude, static_cast<int128_t>(base.lot_size.raw), &units) ||
        __builtin_mul_overflow(units, static_cast<int128_t>(base.contract_value.raw), &units)) {
        return FixedE9::undef();
    }
    return internal::narrow(units / k_e9);
}

FixedE9 short_worst_case(const OptionInstrument& option, QtyLots lots, FixedE9 underlying) noexcept {
    const auto units = option_units(option.base, lots);
    if (option.put_call == PutCall::Put) {
        return internal::mul(option.base.ticks_to_price(option.strike), units);
    }
    return internal::usable(underlying) && underlying.raw > 0 ? internal::mul(underlying, units) : FixedE9::undef();
}

ShortFigures short_figures(const ShortLine& line, const ShortMarginRates& rates) noexcept {
    ShortFigures out{};
    if (line.option == nullptr) {
        return {.naked_usd = FixedE9::undef(), .margin_held = FixedE9::undef(), .margin_working = FixedE9::undef()};
    }
    const auto held = std::max<std::int64_t>(0, -line.position.raw);
    const auto after = std::max<std::int64_t>(0, line.working_sell.raw - line.position.raw);
    out.held = QtyLots::from_raw(held);
    out.working = QtyLots::from_raw(after - held);
    if (after == 0) {
        return out;
    }
    const auto underlying = line.index.is_undef() ? line.forward : line.index;
    out.naked_usd = short_worst_case(*line.option, QtyLots::from_raw(after), underlying);
    const auto margin = short_margin(rates,
                                     {.premium = line.option->premium,
                                      .put_call = line.option->put_call,
                                      .strike = line.option->base.ticks_to_price(line.option->strike),
                                      .index = underlying,
                                      .forward = line.forward.is_undef() ? line.index : line.forward,
                                      .mark = line.mark,
                                      .order_price = line.mark});
    if (!margin.has_value()) {
        out.margin_held = FixedE9::undef();
        out.margin_working = FixedE9::undef();
        return out;
    }
    out.margin_held = internal::mul(margin->initial, option_units(line.option->base, out.held));
    out.margin_working = internal::mul(margin->initial, option_units(line.option->base, out.working));
    return out;
}

std::string_view settlement_asset(const OptionInstrument& option) noexcept {
    if (option.premium == OptionUnit::Base) {
        return option.base.underlying.view();
    }
    if (option.premium != OptionUnit::Quote) {
        return {};
    }
    const auto symbol = option.base.exchange_symbol.view();
    const bool usdt = symbol.find("USDT") != std::string_view::npos;
    const bool usdc = symbol.find("USDC") != std::string_view::npos;
    switch (option.base.id.venue()) {
    case VenueId::Deribit:
        return usdt ? "USDT" : "USDC";
    case VenueId::Bybit:
        // a symbol with no suffix is the older usdc contract
        return usdt ? "USDT" : "USDC";
    case VenueId::Okx:
    case VenueId::Binance:
    case VenueId::Gate:
        return usdc ? "USDC" : "USDT";
    default:
        return {};
    }
}

FixedE9 margin_available(std::span<const BalanceReport> rows,
                         std::string_view asset,
                         UnixNanos pulled,
                         UnixNanos now,
                         std::uint64_t max_age_ns) noexcept {
    if (now.ns > pulled.ns && now.ns - pulled.ns > max_age_ns) {
        return FixedE9::undef();
    }
    for (const auto& row : rows) {
        if (row.asset.view() == asset) {
            return row.available;
        }
    }
    return FixedE9::undef();
}

} // namespace raikiri::oms
