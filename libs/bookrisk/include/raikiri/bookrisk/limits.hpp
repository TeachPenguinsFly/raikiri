#pragma once

// what a person set for a book, and what that comes to in dollars; pure

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/oms/risk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::bookrisk {

// a strategy id today; a book registry maps its names onto the same number
using BookId = std::uint16_t;

enum class Param : std::uint8_t {
    OrderNotional,
    InstrumentNotional,
    BookNotional,
    OptionPremium,
    Loss1m,
    Loss5m,
    Loss1h,
    DailyLoss,
    OptionOpenLots,
    OpenOrders,
    PriceBandBp,
    OrdersInWindow,
    OrderWindowMs,
    RepeatEntries,
    RepeatWindowMs,
    QuoteAgeMs,
};

inline constexpr std::size_t k_param_count = 16;

// the most a sliding count holds, so a limit past it could never fire
inline constexpr std::size_t k_window_capacity = 1024;

struct ParamInfo {
    Param param{};
    std::string_view name{};
    // dollars, so it may be set as a percent of capital
    bool money{false};
    bool may_be_off{true};
    // a longer window counts more against the same limit, so for those a lower value is the raise
    bool higher_is_looser{true};
};

inline constexpr std::array<ParamInfo, k_param_count> k_params{{
    {.param = Param::OrderNotional, .name = "order_notional", .money = true},
    {.param = Param::InstrumentNotional, .name = "instrument_notional", .money = true},
    {.param = Param::BookNotional, .name = "book_notional", .money = true},
    {.param = Param::OptionPremium, .name = "option_premium", .money = true},
    {.param = Param::Loss1m, .name = "loss_1m", .money = true},
    {.param = Param::Loss5m, .name = "loss_5m", .money = true},
    {.param = Param::Loss1h, .name = "loss_1h", .money = true},
    {.param = Param::DailyLoss, .name = "daily_loss", .money = true},
    {.param = Param::OptionOpenLots, .name = "option_open_lots"},
    {.param = Param::OpenOrders, .name = "open_orders", .may_be_off = false},
    {.param = Param::PriceBandBp, .name = "price_band_bp"},
    {.param = Param::OrdersInWindow, .name = "orders_in_window"},
    {.param = Param::OrderWindowMs, .name = "order_window_ms", .may_be_off = false, .higher_is_looser = false},
    {.param = Param::RepeatEntries, .name = "repeat_entries"},
    {.param = Param::RepeatWindowMs, .name = "repeat_window_ms", .may_be_off = false, .higher_is_looser = false},
    {.param = Param::QuoteAgeMs, .name = "quote_age_ms"},
}};

[[nodiscard]] constexpr const ParamInfo& info_of(Param param) noexcept {
    return k_params[static_cast<std::size_t>(param)];
}

[[nodiscard]] std::optional<Param> param_from(std::string_view name) noexcept;

// inherit passes to the row below: book, then defaults, then the risk file
enum class LimitMode : std::uint8_t { Inherit, Off, Absolute, Percent };

[[nodiscard]] std::string_view to_string(LimitMode mode) noexcept;
[[nodiscard]] std::optional<LimitMode> limit_mode_from(std::string_view text) noexcept;

struct Limit {
    LimitMode mode{LimitMode::Inherit};
    // dollars or a whole count for absolute, percent of capital for percent (2.5 is 2.5%)
    FixedE9 value{FixedE9::undef()};

    friend constexpr bool operator==(const Limit&, const Limit&) noexcept = default;
};

// off by default; what a daily loss past its level does on its own
enum class LossAction : std::uint8_t { Off, Reducing, Halted };

[[nodiscard]] std::string_view to_string(LossAction action) noexcept;
[[nodiscard]] std::optional<LossAction> loss_action_from(std::string_view text) noexcept;

struct LimitRow {
    std::array<Limit, k_param_count> limits{};
    // unset inherits; there is nothing below defaults, so it ends at off
    std::optional<LossAction> loss_action{};

    [[nodiscard]] Limit& operator[](Param param) noexcept { return limits[static_cast<std::size_t>(param)]; }

    [[nodiscard]] const Limit& operator[](Param param) const noexcept {
        return limits[static_cast<std::size_t>(param)];
    }

    friend constexpr bool operator==(const LimitRow&, const LimitRow&) noexcept = default;
};

enum class LimitSource : std::uint8_t { File, Defaults, Book };

[[nodiscard]] std::string_view to_string(LimitSource source) noexcept;

struct Resolved {
    // the row that decided, and how it was written there; file means an absolute from the risk file
    LimitSource source{LimitSource::File};
    LimitMode mode{LimitMode::Absolute};
    // dollars or the count; undef is off, or a percent with no capital to take it of
    FixedE9 effective{FixedE9::undef()};
    // of capital; undef unless a money limit and capital are both known
    FixedE9 percent{FixedE9::undef()};
};

struct ResolvedRow {
    std::array<Resolved, k_param_count> params{};
    LossAction loss_action{LossAction::Off};

    [[nodiscard]] const Resolved& operator[](Param param) const noexcept {
        return params[static_cast<std::size_t>(param)];
    }
};

[[nodiscard]] ResolvedRow
resolve(const oms::RiskConfig& base, const LimitRow& defaults, const LimitRow& book, FixedE9 capital) noexcept;

// `base` with every resolved value written over it; loss levels are not the gate's
[[nodiscard]] oms::RiskConfig to_config(oms::RiskConfig base, const ResolvedRow& row) noexcept;

// empty when acceptable
[[nodiscard]] std::string check(Param param, const Limit& limit);

// whether moving from `before` to `after` lets more through; undef is off
[[nodiscard]] bool loosens(Param param, FixedE9 before, FixedE9 after) noexcept;

// names every param `after` lets more through than `before`
void loosened(const ResolvedRow& before, const ResolvedRow& after, std::vector<std::string>& out);

// fixed-point percent arithmetic; undef past the range rather than an abort, since a person typed the operands
[[nodiscard]] FixedE9 share_of(FixedE9 capital, FixedE9 percent) noexcept;
[[nodiscard]] FixedE9 percent_of(FixedE9 part, FixedE9 capital) noexcept;

} // namespace raikiri::bookrisk
