#pragma once

#include <raikiri/oms/risk.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // options: premium per order in whole quote units, and open contracts per instrument in lots
    std::optional<std::int64_t> max_option_premium{};
    std::optional<std::int64_t> max_option_open_lots{};
    // per book: orders sent in a sliding window, fill-then-re-entry cycles in a window, age of the quote an order
    // leans on; unset or zero is off
    std::optional<std::uint32_t> max_orders_in_window{};
    std::optional<std::uint64_t> order_window_ms{};
    std::optional<std::uint32_t> max_repeat_entries{};
    std::optional<std::uint64_t> repeat_window_ms{};
    std::optional<std::uint64_t> max_quote_age_ms{};
    // how long an identical order counts as a duplicate; 0 lets a book repeat itself on purpose
    std::optional<std::int64_t> duplicate_window_ms{};
    // short options: whole usd, and the spare share a short opening must leave, in bp
    std::optional<std::int64_t> max_naked_short{};
    std::optional<std::int64_t> max_short_vega{};
    std::optional<std::int64_t> max_short_gamma{};
    std::optional<std::int64_t> short_margin_buffer_bp{};
};

struct CrossBookSettings {
    bool enabled{false};
    std::int64_t fee_bp{0};
    std::vector<std::string> instruments{};
};

// the desk matching its own clients: off unless named here or on the risk page, whose saved word then wins
struct CrossSettings {
    bool enabled{false};
    std::int64_t max_spread_bp{20};
    std::uint64_t max_quote_age_ms{1000};
    std::uint64_t hold_ms{0};
    bool against_desk{false};
    std::map<std::string, CrossBookSettings> books{};
};

// `strategies` is keyed by strategy name or id; a set field there replaces the default
struct RiskFile {
    RiskSettings defaults{};
    std::map<std::string, RiskSettings> strategies{};
    // a credential reference (env:, file:, op:), never the url itself; unset sends nothing
    std::optional<std::string> notify_webhook{};
    CrossSettings internal_cross{};
};

[[nodiscard]] std::expected<RiskFile, std::string> load_risk(std::string_view path);

// the name's entry wins over the id's
[[nodiscard]] RiskSettings risk_for(const RiskFile& file, std::string_view name, std::uint16_t strategy_id);

[[nodiscard]] oms::RiskConfig to_risk_config(const RiskSettings& settings) noexcept;

} // namespace raikiri::config
