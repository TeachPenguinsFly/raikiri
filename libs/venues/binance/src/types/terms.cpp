#include <raikiri/venues/binance/terms.hpp>
#include <raikiri/wire/decimal.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::binance {

namespace {

using Tiers = std::vector<MaintenanceTier>;

// nullopt for another symbol's entry
std::expected<std::optional<Tiers>, WireError> read_brackets(wire::ondemand::object& obj, std::string_view symbol) {
    const auto named = wire::req_str(obj, "symbol");
    if (!named.has_value()) {
        return std::unexpected(named.error());
    }
    if (*named != symbol) {
        return std::optional<Tiers>{};
    }
    auto brackets = wire::req_array(obj, "brackets");
    if (!brackets.has_value()) {
        return std::unexpected(brackets.error());
    }
    Tiers out;
    for (auto entry : *brackets) {
        wire::ondemand::object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto floor = wire::decimal_field(row, "notionalFloor");
        const auto cap = wire::decimal_field(row, "notionalCap");
        const auto rate = wire::decimal_field(row, "maintMarginRatio");
        const auto amount = wire::decimal_field(row, "cum", 0.0);
        const auto leverage = wire::decimal_field(row, "initialLeverage", 0.0);
        for (const auto* field : {&floor, &cap, &rate, &amount, &leverage}) {
            if (!field->has_value()) {
                return std::unexpected(field->error());
            }
        }
        out.push_back({.floor = *floor, .cap = *cap, .rate = *rate, .amount = *amount, .max_leverage = *leverage});
    }
    return std::optional<Tiers>{std::move(out)};
}

} // namespace

std::expected<WireCommissionRate, WireError> parse_commission_rate(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireCommissionRate, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto maker = wire::decimal_field(*root, "makerCommissionRate");
        if (!maker.has_value()) {
            return std::unexpected(maker.error());
        }
        const auto taker = wire::decimal_field(*root, "takerCommissionRate");
        if (!taker.has_value()) {
            return std::unexpected(taker.error());
        }
        return WireCommissionRate{.maker = *maker, .taker = *taker};
    });
}

std::expected<Tiers, WireError> parse_leverage_brackets(std::string_view body, std::string_view symbol) {
    return wire::guarded([&]() -> std::expected<Tiers, WireError> {
        if (const auto text = wire::trim(body); !text.empty() && text.front() == '{') {
            auto root = wire::session().open_object(body);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            auto found = read_brackets(*root, symbol);
            if (!found.has_value()) {
                return std::unexpected(found.error());
            }
            if (found->has_value()) {
                return std::move(**found);
            }
            return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
        }
        auto array = wire::session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            auto found = read_brackets(obj, symbol);
            if (!found.has_value()) {
                return std::unexpected(found.error());
            }
            if (found->has_value()) {
                return std::move(**found);
            }
        }
        return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
    });
}

} // namespace raikiri::venues::binance
