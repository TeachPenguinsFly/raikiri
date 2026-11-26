#include <raikiri/venues/bybit/terms.hpp>
#include <raikiri/wire/decimal.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

std::expected<WireFeeRate, WireError> parse_fee_rate(std::string_view result, std::string_view symbol) {
    return wire::guarded([&]() -> std::expected<WireFeeRate, WireError> {
        auto root = wire::session().open_object(result);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto list = wire::req_array(*root, "list");
        if (!list.has_value()) {
            return std::unexpected(list.error());
        }
        for (auto entry : *list) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto named = wire::opt_str(obj, "symbol");
            if (!named.has_value()) {
                return std::unexpected(named.error());
            }
            if (!named->empty() && *named != symbol) {
                continue;
            }
            const auto maker = wire::decimal_field(obj, "makerFeeRate");
            if (!maker.has_value()) {
                return std::unexpected(maker.error());
            }
            const auto taker = wire::decimal_field(obj, "takerFeeRate");
            if (!taker.has_value()) {
                return std::unexpected(taker.error());
            }
            return WireFeeRate{.maker = *maker, .taker = *taker};
        }
        return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
    });
}

std::expected<std::vector<MaintenanceTier>, WireError> parse_risk_limit(std::string_view result,
                                                                        std::string_view symbol) {
    return wire::guarded([&]() -> std::expected<std::vector<MaintenanceTier>, WireError> {
        auto root = wire::session().open_object(result);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto list = wire::req_array(*root, "list");
        if (!list.has_value()) {
            return std::unexpected(list.error());
        }
        std::vector<MaintenanceTier> out;
        double floor = 0;
        for (auto entry : *list) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto named = wire::req_str(obj, "symbol");
            if (!named.has_value()) {
                return std::unexpected(named.error());
            }
            if (*named != symbol) {
                continue;
            }
            const auto cap = wire::decimal_field(obj, "riskLimitValue");
            const auto rate = wire::decimal_field(obj, "maintenanceMargin");
            const auto amount = wire::decimal_field(obj, "mmDeduction", 0.0);
            const auto leverage = wire::decimal_field(obj, "maxLeverage", 0.0);
            for (const auto* field : {&cap, &rate, &amount, &leverage}) {
                if (!field->has_value()) {
                    return std::unexpected(field->error());
                }
            }
            out.push_back({.floor = floor, .cap = *cap, .rate = *rate, .amount = *amount, .max_leverage = *leverage});
            floor = *cap;
        }
        return out;
    });
}

} // namespace raikiri::venues::bybit
