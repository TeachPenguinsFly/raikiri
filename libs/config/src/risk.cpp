#include <raikiri/config/risk.hpp>

#include <array>
#include <format>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <limits>
#include <utility>

namespace raikiri::config {
namespace internal {

constexpr std::int64_t k_max_whole = std::numeric_limits<std::int64_t>::max() / k_e9;
constexpr std::uint64_t k_ms_ns = 1'000'000;

template <typename T> void overlay(std::optional<T>& into, const std::optional<T>& from) {
    if (from.has_value()) {
        into = from;
    }
}

[[nodiscard]] std::string check(const RiskSettings& settings) {
    const std::array<std::pair<std::string_view, const std::optional<std::int64_t>*>, 8> positives{{
        {"max_order_notional", &settings.max_order_notional},
        {"max_instrument_notional", &settings.max_instrument_notional},
        {"max_strategy_notional", &settings.max_strategy_notional},
        {"max_price_band_bp", &settings.max_price_band_bp},
        {"max_option_premium", &settings.max_option_premium},
        {"max_naked_short", &settings.max_naked_short},
        {"max_short_vega", &settings.max_short_vega},
        {"max_short_gamma", &settings.max_short_gamma},
    }};
    for (const auto& [name, slot] : positives) {
        const auto& value = *slot;
        if (value.has_value() && (*value <= 0 || *value > k_max_whole)) {
            return std::string{name} + " must be positive and below " + std::to_string(k_max_whole);
        }
    }
    for (const auto* lots : {&settings.max_lots.spot,
                             &settings.max_lots.perp,
                             &settings.max_lots.future,
                             &settings.max_lots.option,
                             &settings.max_option_open_lots}) {
        if (lots->has_value() && **lots <= 0) {
            return "a lot ceiling must be positive";
        }
    }
    if (settings.duplicate_window_ms.has_value() && *settings.duplicate_window_ms < 0) {
        return "duplicate_window_ms is negative";
    }
    if (settings.short_margin_buffer_bp.has_value() && *settings.short_margin_buffer_bp < 0) {
        return "short_margin_buffer_bp is negative";
    }
    if (settings.max_open_orders.has_value() && *settings.max_open_orders == 0) {
        return "max_open_orders is 0";
    }
    for (const auto* window : {&settings.order_window_ms, &settings.repeat_window_ms}) {
        if (window->has_value() && **window == 0) {
            return "a window must be longer than zero";
        }
    }
    constexpr std::uint64_t k_max_ms = std::numeric_limits<std::int64_t>::max() / k_ms_ns;
    for (const auto* span : {&settings.order_window_ms, &settings.repeat_window_ms, &settings.max_quote_age_ms}) {
        if (span->has_value() && **span > k_max_ms) {
            return "a window or age is too long";
        }
    }
    return {};
}

[[nodiscard]] FixedE9 whole_e9(const std::optional<std::int64_t>& units) noexcept {
    return units.has_value() ? FixedE9::from_raw(*units * k_e9) : FixedE9::undef();
}

[[nodiscard]] QtyLots lots(const std::optional<std::int64_t>& value) noexcept {
    return value.has_value() ? QtyLots::from_raw(*value) : QtyLots::undef();
}

} // namespace internal

std::expected<RiskFile, std::string> load_risk(std::string_view path) {
    RiskFile out{};
    if (const auto ec = glz::read_file_yaml(out, path); ec) {
        return std::unexpected{std::string{path} + ": " + glz::format_error(ec)};
    }
    if (auto why = internal::check(out.defaults); !why.empty()) {
        return std::unexpected{std::string{path} + " defaults: " + why};
    }
    for (const auto& [key, settings] : out.strategies) {
        if (auto why = internal::check(settings); !why.empty()) {
            return std::unexpected{std::format("{} strategy {}: {}", path, key, why)};
        }
    }
    return out;
}

RiskSettings risk_for(const RiskFile& file, std::string_view name, std::uint16_t strategy_id) {
    auto it = file.strategies.find(std::string{name});
    if (it == file.strategies.end()) {
        it = file.strategies.find(std::to_string(strategy_id));
    }
    RiskSettings out = file.defaults;
    if (it == file.strategies.end()) {
        return out;
    }
    const auto& over = it->second;
    internal::overlay(out.max_open_orders, over.max_open_orders);
    internal::overlay(out.max_price_band_bp, over.max_price_band_bp);
    internal::overlay(out.max_order_notional, over.max_order_notional);
    internal::overlay(out.max_instrument_notional, over.max_instrument_notional);
    internal::overlay(out.max_strategy_notional, over.max_strategy_notional);
    internal::overlay(out.max_lots.spot, over.max_lots.spot);
    internal::overlay(out.max_lots.perp, over.max_lots.perp);
    internal::overlay(out.max_lots.future, over.max_lots.future);
    internal::overlay(out.max_lots.option, over.max_lots.option);
    internal::overlay(out.max_option_premium, over.max_option_premium);
    internal::overlay(out.max_option_open_lots, over.max_option_open_lots);
    internal::overlay(out.max_orders_in_window, over.max_orders_in_window);
    internal::overlay(out.order_window_ms, over.order_window_ms);
    internal::overlay(out.max_repeat_entries, over.max_repeat_entries);
    internal::overlay(out.repeat_window_ms, over.repeat_window_ms);
    internal::overlay(out.max_quote_age_ms, over.max_quote_age_ms);
    internal::overlay(out.duplicate_window_ms, over.duplicate_window_ms);
    internal::overlay(out.max_naked_short, over.max_naked_short);
    internal::overlay(out.max_short_vega, over.max_short_vega);
    internal::overlay(out.max_short_gamma, over.max_short_gamma);
    internal::overlay(out.short_margin_buffer_bp, over.short_margin_buffer_bp);
    return out;
}

oms::RiskConfig to_risk_config(const RiskSettings& settings) noexcept {
    oms::RiskConfig out{};
    if (settings.max_open_orders.has_value()) {
        out.max_open_orders = *settings.max_open_orders;
    }
    out.max_price_band_bp = settings.max_price_band_bp.value_or(0);
    if (settings.duplicate_window_ms.has_value()) {
        out.duplicate_window_ns = static_cast<std::uint64_t>(*settings.duplicate_window_ms) * 1'000'000ULL;
    }
    out.max_order_notional = internal::whole_e9(settings.max_order_notional);
    out.max_instrument_notional = internal::whole_e9(settings.max_instrument_notional);
    out.max_strategy_notional = internal::whole_e9(settings.max_strategy_notional);
    out.max_lots = {.spot = internal::lots(settings.max_lots.spot),
                    .perp = internal::lots(settings.max_lots.perp),
                    .future = internal::lots(settings.max_lots.future),
                    .option = internal::lots(settings.max_lots.option)};
    out.max_option_premium = internal::whole_e9(settings.max_option_premium);
    out.max_option_open_lots = internal::lots(settings.max_option_open_lots);
    out.max_orders_in_window = settings.max_orders_in_window.value_or(0);
    out.max_repeat_entries = settings.max_repeat_entries.value_or(0);
    out.max_quote_age_ns = settings.max_quote_age_ms.value_or(0) * internal::k_ms_ns;
    if (settings.order_window_ms.has_value()) {
        out.order_window_ns = *settings.order_window_ms * internal::k_ms_ns;
    }
    if (settings.repeat_window_ms.has_value()) {
        out.repeat_window_ns = *settings.repeat_window_ms * internal::k_ms_ns;
    }
    out.max_naked_short = internal::whole_e9(settings.max_naked_short);
    out.max_short_vega = internal::whole_e9(settings.max_short_vega);
    out.max_short_gamma = internal::whole_e9(settings.max_short_gamma);
    out.short_margin_buffer_bp = settings.short_margin_buffer_bp.value_or(out.short_margin_buffer_bp);
    return out;
}

} // namespace raikiri::config
