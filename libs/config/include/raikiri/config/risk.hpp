#pragma once

#include <raikiri/oms/risk.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::config {

struct LotCeilingSettings {
    std::optional<std::int64_t> spot{};
    std::optional<std::int64_t> perp{};
    std::optional<std::int64_t> future{};
    std::optional<std::int64_t> option{};
};

// notionals are whole units of the instrument's quote currency; unset is off
struct RiskSettings {
    std::optional<std::uint32_t> max_open_orders{};
    std::optional<std::int64_t> max_price_band_bp{};
    std::optional<std::int64_t> max_order_notional{};
    std::optional<std::int64_t> max_instrument_notional{};
    std::optional<std::int64_t> max_strategy_notional{};
    LotCeilingSettings max_lots{};
};

// `strategies` is keyed by strategy name or id; a set field there replaces the default
struct RiskFile {
    RiskSettings defaults{};
    std::map<std::string, RiskSettings> strategies{};
};

[[nodiscard]] std::expected<RiskFile, std::string> load_risk(std::string_view path);

// the name's entry wins over the id's
[[nodiscard]] RiskSettings risk_for(const RiskFile& file, std::string_view name, std::uint16_t strategy_id);

[[nodiscard]] oms::RiskConfig to_risk_config(const RiskSettings& settings) noexcept;

} // namespace raikiri::config
