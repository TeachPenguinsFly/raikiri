#include <raikiri/venues/hyperliquid/terms.hpp>
#include <raikiri/wire/decimal.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::int64_t k_first_table_id = 50;

struct Asset {
    std::int64_t table{-1};
    double max_leverage{0};
};

void finish(std::vector<MaintenanceTier>& tiers) noexcept {
    for (std::size_t i = 0; i + 1 < tiers.size(); ++i) {
        tiers[i].cap = tiers[i + 1].floor;
    }
    fill_maintenance_amounts(tiers);
}

} // namespace

std::expected<std::vector<MaintenanceTier>, WireError> parse_margin_tiers(std::string_view meta,
                                                                          std::string_view coin) {
    return wire::guarded([&]() -> std::expected<std::vector<MaintenanceTier>, WireError> {
        auto root = wire::session().open_object(meta);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto universe = wire::req_array(*root, "universe");
        if (!universe.has_value()) {
            return std::unexpected(universe.error());
        }
        Asset asset{};
        bool found = false;
        for (auto entry : *universe) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto name = wire::req_str(obj, "name");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            if (found || *name != coin) {
                continue;
            }
            const auto leverage = wire::decimal_field(obj, "maxLeverage");
            if (!leverage.has_value()) {
                return std::unexpected(leverage.error());
            }
            const auto table = wire::opt_i64(obj, "marginTableId");
            if (!table.has_value()) {
                return std::unexpected(table.error());
            }
            asset = {.table = *table, .max_leverage = *leverage};
            found = true;
        }
        if (!found || asset.max_leverage <= 0) {
            return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
        }
        std::vector<MaintenanceTier> out;
        if (asset.table < k_first_table_id) {
            out.push_back({.floor = 0.0,
                           .cap = 0.0,
                           .rate = 1.0 / (2.0 * asset.max_leverage),
                           .amount = 0.0,
                           .max_leverage = asset.max_leverage});
            return out;
        }
        auto tables = wire::req_array(*root, "marginTables");
        if (!tables.has_value()) {
            return std::unexpected(tables.error());
        }
        // each table is a pair: [id, {description, marginTiers}]
        for (auto entry : *tables) {
            wire::ondemand::array pair;
            if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            std::int64_t id = -1;
            bool wanted = false;
            std::size_t index = 0;
            for (auto element : pair) {
                wire::ondemand::value value;
                if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                if (index == 0) {
                    const auto read = wire::i64_of(value);
                    if (!read.has_value()) {
                        return std::unexpected(read.error());
                    }
                    id = *read;
                    wanted = id == asset.table;
                } else if (index == 1 && wanted) {
                    wire::ondemand::object body;
                    if (const auto ec = value.get_object().get(body); ec != simdjson::SUCCESS) {
                        return std::unexpected(wire::map_error(ec));
                    }
                    auto rows = wire::req_array(body, "marginTiers");
                    if (!rows.has_value()) {
                        return std::unexpected(rows.error());
                    }
                    for (auto row : *rows) {
                        wire::ondemand::object tier;
                        if (const auto ec = row.get_object().get(tier); ec != simdjson::SUCCESS) {
                            return std::unexpected(wire::map_error(ec));
                        }
                        const auto floor = wire::decimal_field(tier, "lowerBound");
                        if (!floor.has_value()) {
                            return std::unexpected(floor.error());
                        }
                        const auto leverage = wire::decimal_field(tier, "maxLeverage");
                        if (!leverage.has_value()) {
                            return std::unexpected(leverage.error());
                        }
                        if (*leverage <= 0) {
                            return std::unexpected(WireError::WrongType);
                        }
                        out.push_back({.floor = *floor,
                                       .cap = 0.0,
                                       .rate = 1.0 / (2.0 * *leverage),
                                       .amount = 0.0,
                                       .max_leverage = *leverage});
                    }
                }
                ++index;
            }
            if (wanted) {
                finish(out);
                return out;
            }
        }
        return std::unexpected(wire::map_error(simdjson::NO_SUCH_FIELD));
    });
}

} // namespace raikiri::venues::hyperliquid
