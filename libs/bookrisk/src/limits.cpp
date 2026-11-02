#include <raikiri/bookrisk/limits.hpp>

#include <algorithm>
#include <limits>

namespace raikiri::bookrisk {
namespace internal {

constexpr std::int64_t k_hundred_e9 = 100 * k_e9;
constexpr std::int64_t k_max_percent = 10'000 * k_e9;
constexpr std::uint64_t k_ms_ns = 1'000'000;
// a count or a span in milliseconds still fits nanoseconds
constexpr std::int64_t k_max_count = std::numeric_limits<std::int64_t>::max() / 1'000'000;

[[nodiscard]] FixedE9 whole(std::int64_t value) noexcept {
    const auto most = std::numeric_limits<std::int64_t>::max() / k_e9;
    return value > most ? FixedE9::undef() : FixedE9::from_raw(value * k_e9);
}

[[nodiscard]] FixedE9 from_file(const oms::RiskConfig& base, Param param) noexcept {
    switch (param) {
    case Param::OrderNotional:
        return base.max_order_notional;
    case Param::InstrumentNotional:
        return base.max_instrument_notional;
    case Param::BookNotional:
        return base.max_strategy_notional;
    case Param::OptionPremium:
        return base.max_option_premium;
    case Param::Loss1m:
    case Param::Loss5m:
    case Param::Loss1h:
    case Param::DailyLoss:
        return FixedE9::undef();
    case Param::OptionOpenLots:
        return base.max_option_open_lots.is_undef() ? FixedE9::undef() : whole(base.max_option_open_lots.raw);
    case Param::OpenOrders:
        return whole(static_cast<std::int64_t>(base.max_open_orders));
    case Param::PriceBandBp:
        return base.max_price_band_bp > 0 ? whole(base.max_price_band_bp) : FixedE9::undef();
    case Param::OrdersInWindow:
        return base.max_orders_in_window > 0 ? whole(base.max_orders_in_window) : FixedE9::undef();
    case Param::OrderWindowMs:
        return whole(static_cast<std::int64_t>(base.order_window_ns / k_ms_ns));
    case Param::RepeatEntries:
        return base.max_repeat_entries > 0 ? whole(base.max_repeat_entries) : FixedE9::undef();
    case Param::RepeatWindowMs:
        return whole(static_cast<std::int64_t>(base.repeat_window_ns / k_ms_ns));
    case Param::QuoteAgeMs:
        return base.max_quote_age_ns > 0 ? whole(static_cast<std::int64_t>(base.max_quote_age_ns / k_ms_ns))
                                         : FixedE9::undef();
    }
    return FixedE9::undef();
}

[[nodiscard]] Resolved
from_row(const Limit& limit, LimitSource source, const ParamInfo& info, FixedE9 capital) noexcept {
    Resolved out{.source = source, .mode = limit.mode};
    if (limit.mode == LimitMode::Absolute) {
        out.effective = limit.value;
    } else if (limit.mode == LimitMode::Percent) {
        out.effective = share_of(capital, limit.value);
    }
    if (info.money) {
        out.percent = limit.mode == LimitMode::Percent ? limit.value : percent_of(out.effective, capital);
    }
    return out;
}

[[nodiscard]] std::int64_t count_of(FixedE9 value, std::int64_t off) noexcept {
    return value.is_undef() ? off : value.raw / k_e9;
}

} // namespace internal

std::optional<Param> param_from(std::string_view name) noexcept {
    for (const auto& info : k_params) {
        if (info.name == name) {
            return info.param;
        }
    }
    return std::nullopt;
}

std::string_view to_string(LimitMode mode) noexcept {
    switch (mode) {
    case LimitMode::Inherit:
        return "inherit";
    case LimitMode::Off:
        return "off";
    case LimitMode::Absolute:
        return "absolute";
    case LimitMode::Percent:
        return "percent";
    }
    return "?";
}

std::optional<LimitMode> limit_mode_from(std::string_view text) noexcept {
    for (const auto mode : {LimitMode::Inherit, LimitMode::Off, LimitMode::Absolute, LimitMode::Percent}) {
        if (to_string(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

std::string_view to_string(LossAction action) noexcept {
    switch (action) {
    case LossAction::Off:
        return "off";
    case LossAction::Reducing:
        return "reducing";
    case LossAction::Halted:
        return "halted";
    }
    return "?";
}

std::optional<LossAction> loss_action_from(std::string_view text) noexcept {
    for (const auto action : {LossAction::Off, LossAction::Reducing, LossAction::Halted}) {
        if (to_string(action) == text) {
            return action;
        }
    }
    return std::nullopt;
}

std::string_view to_string(LimitSource source) noexcept {
    switch (source) {
    case LimitSource::File:
        return "file";
    case LimitSource::Defaults:
        return "defaults";
    case LimitSource::Book:
        return "book";
    }
    return "?";
}

FixedE9 share_of(FixedE9 capital, FixedE9 percent) noexcept {
    if (capital.is_undef() || percent.is_undef()) {
        return FixedE9::undef();
    }
    const auto out = static_cast<int128_t>(capital.raw) * percent.raw / internal::k_hundred_e9;
    return out >= std::numeric_limits<std::int64_t>::max() ? FixedE9::undef()
                                                           : FixedE9::from_raw(static_cast<std::int64_t>(out));
}

FixedE9 percent_of(FixedE9 part, FixedE9 capital) noexcept {
    if (part.is_undef() || capital.is_undef() || capital.raw <= 0) {
        return FixedE9::undef();
    }
    const auto out = static_cast<int128_t>(part.raw) * internal::k_hundred_e9 / capital.raw;
    return out >= std::numeric_limits<std::int64_t>::max() || out <= std::numeric_limits<std::int64_t>::min()
               ? FixedE9::undef()
               : FixedE9::from_raw(static_cast<std::int64_t>(out));
}

ResolvedRow
resolve(const oms::RiskConfig& base, const LimitRow& defaults, const LimitRow& book, FixedE9 capital) noexcept {
    ResolvedRow out{};
    for (const auto& info : k_params) {
        auto& slot = out.params[static_cast<std::size_t>(info.param)];
        if (book[info.param].mode != LimitMode::Inherit) {
            slot = internal::from_row(book[info.param], LimitSource::Book, info, capital);
        } else if (defaults[info.param].mode != LimitMode::Inherit) {
            slot = internal::from_row(defaults[info.param], LimitSource::Defaults, info, capital);
        } else {
            slot = internal::from_row({.mode = LimitMode::Absolute, .value = internal::from_file(base, info.param)},
                                      LimitSource::File,
                                      info,
                                      capital);
            if (slot.effective.is_undef()) {
                slot.mode = LimitMode::Off;
            }
        }
    }
    out.loss_action = book.loss_action.value_or(defaults.loss_action.value_or(LossAction::Off));
    return out;
}

oms::RiskConfig to_config(oms::RiskConfig base, const ResolvedRow& row) noexcept {
    const auto money = [&row](Param param) { return row[param].effective; };
    const auto count = [&row](Param param, std::int64_t off = 0) {
        return internal::count_of(row[param].effective, off);
    };
    base.max_order_notional = money(Param::OrderNotional);
    base.max_instrument_notional = money(Param::InstrumentNotional);
    base.max_strategy_notional = money(Param::BookNotional);
    base.max_option_premium = money(Param::OptionPremium);
    base.max_option_open_lots = row[Param::OptionOpenLots].effective.is_undef()
                                    ? QtyLots::undef()
                                    : QtyLots::from_raw(count(Param::OptionOpenLots));
    base.max_open_orders = static_cast<std::size_t>(count(Param::OpenOrders, 1));
    base.max_price_band_bp = count(Param::PriceBandBp);
    base.max_orders_in_window = static_cast<std::uint32_t>(
        std::min<std::int64_t>(count(Param::OrdersInWindow), static_cast<std::int64_t>(k_window_capacity)));
    base.order_window_ns = static_cast<std::uint64_t>(count(Param::OrderWindowMs, 1'000)) * internal::k_ms_ns;
    base.max_repeat_entries = static_cast<std::uint32_t>(
        std::min<std::int64_t>(count(Param::RepeatEntries), static_cast<std::int64_t>(k_window_capacity)));
    base.repeat_window_ns = static_cast<std::uint64_t>(count(Param::RepeatWindowMs, 60'000)) * internal::k_ms_ns;
    base.max_quote_age_ns = static_cast<std::uint64_t>(count(Param::QuoteAgeMs)) * internal::k_ms_ns;
    return base;
}

std::string check(Param param, const Limit& limit) {
    const auto& info = info_of(param);
    const std::string name{info.name};
    switch (limit.mode) {
    case LimitMode::Inherit:
        return {};
    case LimitMode::Off:
        return info.may_be_off ? std::string{} : name + " cannot be off";
    case LimitMode::Percent:
        if (!info.money) {
            return name + " is a count, not a share of capital";
        }
        if (limit.value.is_undef() || limit.value.raw <= 0 || limit.value.raw > internal::k_max_percent) {
            return name + " percent must be above 0 and at most 10000";
        }
        return {};
    case LimitMode::Absolute:
        if (limit.value.is_undef() || limit.value.raw <= 0) {
            return name + " must be above 0";
        }
        if (!info.money && limit.value.raw % k_e9 != 0) {
            return name + " must be a whole number";
        }
        if (!info.money && limit.value.raw / k_e9 > internal::k_max_count) {
            return name + " is too large";
        }
        if ((param == Param::OrdersInWindow || param == Param::RepeatEntries) &&
            limit.value.raw / k_e9 > static_cast<std::int64_t>(k_window_capacity)) {
            return name + " can be at most " + std::to_string(k_window_capacity);
        }
        return {};
    }
    return "unknown mode";
}

bool loosens(Param param, FixedE9 before, FixedE9 after) noexcept {
    if (before == after) {
        return false;
    }
    if (after.is_undef()) {
        return true;
    }
    if (before.is_undef()) {
        return false;
    }
    return info_of(param).higher_is_looser ? after > before : after < before;
}

void loosened(const ResolvedRow& before, const ResolvedRow& after, std::vector<std::string>& out) {
    for (const auto& info : k_params) {
        if (loosens(info.param, before[info.param].effective, after[info.param].effective)) {
            std::string name{info.name};
            if (std::ranges::find(out, name) == out.end()) {
                out.push_back(std::move(name));
            }
        }
    }
    if (static_cast<int>(after.loss_action) < static_cast<int>(before.loss_action)) {
        if (std::string name{"daily_loss_action"}; std::ranges::find(out, name) == out.end()) {
            out.push_back(std::move(name));
        }
    }
}

} // namespace raikiri::bookrisk
