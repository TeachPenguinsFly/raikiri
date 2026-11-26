#include <raikiri/venues/okx/terms.hpp>
#include <raikiri/wire/decimal.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>

namespace raikiri::venues::okx {

namespace {

std::expected<std::optional<WireTradeFee::Pair>, WireError>
read_pair(wire::ondemand::object& obj, std::string_view maker_key, std::string_view taker_key) {
    constexpr double k_absent = 2.0; // no rate is 200%
    const auto maker = wire::decimal_field(obj, maker_key, k_absent);
    if (!maker.has_value()) {
        return std::unexpected(maker.error());
    }
    const auto taker = wire::decimal_field(obj, taker_key, k_absent);
    if (!taker.has_value()) {
        return std::unexpected(taker.error());
    }
    if (*maker == k_absent || *taker == k_absent) {
        return std::optional<WireTradeFee::Pair>{};
    }
    return std::optional{WireTradeFee::Pair{.maker = -*maker, .taker = -*taker}};
}

} // namespace

std::expected<WireTradeFee, WireError> parse_trade_fee(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireTradeFee, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        for (auto entry : *data) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTradeFee out{};
            const auto coin = read_pair(obj, "maker", "taker");
            const auto usdt = read_pair(obj, "makerU", "takerU");
            const auto usdc = read_pair(obj, "makerUSDC", "takerUSDC");
            for (const auto* pair : {&coin, &usdt, &usdc}) {
                if (!pair->has_value()) {
                    return std::unexpected(pair->error());
                }
            }
            out.coin = *coin;
            out.usdt = *usdt;
            out.usdc = *usdc;
            return out;
        }
        return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
    });
}

std::expected<std::vector<MaintenanceTier>, WireError> parse_position_tiers(std::string_view body) {
    return wire::guarded([&]() -> std::expected<std::vector<MaintenanceTier>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_array(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        std::vector<MaintenanceTier> out;
        for (auto entry : *data) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto floor = wire::decimal_field(obj, "minSz");
            const auto cap = wire::decimal_field(obj, "maxSz", 0.0);
            const auto rate = wire::decimal_field(obj, "mmr");
            const auto leverage = wire::decimal_field(obj, "maxLever", 0.0);
            for (const auto* field : {&floor, &cap, &rate, &leverage}) {
                if (!field->has_value()) {
                    return std::unexpected(field->error());
                }
            }
            out.push_back({.floor = *floor, .cap = *cap, .rate = *rate, .amount = 0.0, .max_leverage = *leverage});
        }
        std::ranges::sort(out, {}, &MaintenanceTier::floor);
        return out;
    });
}

std::string_view inst_family_of(std::string_view inst_id) noexcept {
    const auto first = inst_id.find('-');
    if (first == std::string_view::npos) {
        return {};
    }
    const auto second = inst_id.find('-', first + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return inst_id.substr(0, second);
}

} // namespace raikiri::venues::okx
