#include <raikiri/config/risk.hpp>

#include <array>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <limits>
#include <utility>

namespace raikiri::config {
namespace internal {

constexpr std::int64_t k_max_whole = std::numeric_limits<std::int64_t>::max() / k_e9;

template <typename T> void overlay(std::optional<T>& into, const std::optional<T>& from) {
    if (from.has_value()) {
        into = from;
    }
}

[[nodiscard]] std::string check(const RiskSettings& settings) {
    const std::array<std::pair<std::string_view, const std::optional<std::int64_t>*>, 4> positives{{
        {"max_order_notional", &settings.max_order_notional},
        {"max_instrument_notional", &settings.max_instrument_notional},
        {"max_strategy_notional", &settings.max_strategy_notional},
        {"max_price_band_bp", &settings.max_price_band_bp},
    }};
    for (const auto& [name, value] : positives) {
        if (value->has_value() && (**value <= 0 || **value > k_max_whole)) {
            return std::string{name} + " must be positive and below " + std::to_string(k_max_whole);
        }
    }
    for (const auto* lots :
         {&settings.max_lots.spot, &settings.max_lots.perp, &settings.max_lots.future, &settings.max_lots.option}) {
        if (lots->has_value() && **lots <= 0) {
            return "a lot ceiling must be positive";
        }
    }
    if (settings.max_open_orders.has_value() && *settings.max_open_orders == 0) {
        return "max_open_orders is 0";
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
            return std::unexpected{std::string{path} + " strategy " + key + ": " + why};
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
    return out;
}

oms::RiskConfig to_risk_config(const RiskSettings& settings) noexcept {
    oms::RiskConfig out{};
    if (settings.max_open_orders.has_value()) {
        out.max_open_orders = *settings.max_open_orders;
    }
    out.max_price_band_bp = settings.max_price_band_bp.value_or(0);
    out.max_order_notional = internal::whole_e9(settings.max_order_notional);
    out.max_instrument_notional = internal::whole_e9(settings.max_instrument_notional);
    out.max_strategy_notional = internal::whole_e9(settings.max_strategy_notional);
    out.max_lots = {.spot = internal::lots(settings.max_lots.spot),
                    .perp = internal::lots(settings.max_lots.perp),
                    .future = internal::lots(settings.max_lots.future),
                    .option = internal::lots(settings.max_lots.option)};
    return out;
}

} // namespace raikiri::config
