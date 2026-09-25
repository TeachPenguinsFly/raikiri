#include <raikiri/venues/extended/catalogue.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::extended {

namespace json = raikiri::wire;

namespace {

std::expected<void, WireError> read_stats(json::ondemand::object& row, WireMarket& out) {
    auto stats = json::req_object(row, "marketStats");
    if (!stats.has_value()) {
        return std::unexpected(stats.error());
    }
    for (const auto& [key, into] : {std::pair{"dailyVolume", &out.volume_quote},
                                    std::pair{"dailyVolumeBase", &out.volume_base},
                                    std::pair{"markPrice", &out.mark_price},
                                    std::pair{"fundingRate", &out.funding_rate}}) {
        const auto value = json::opt_str(*stats, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const auto next = json::opt_u64(*stats, "nextFundingRate");
    if (!next.has_value()) {
        return std::unexpected(next.error());
    }
    out.next_funding_ms = *next;
    return {};
}

std::expected<void, WireError> read_config(json::ondemand::object& row, WireMarket& out) {
    auto config = json::req_object(row, "tradingConfig");
    if (!config.has_value()) {
        return std::unexpected(config.error());
    }
    for (const auto& [key, into] : {std::pair{"minOrderSize", &out.min_size},
                                    std::pair{"minOrderSizeChange", &out.lot},
                                    std::pair{"minPriceChange", &out.tick}}) {
        const auto value = json::req_str(*config, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    return {};
}

std::expected<WireMarket, WireError> read_market(json::ondemand::object& row) {
    WireMarket out{};
    for (const auto& [key, into] : {std::pair{"name", &out.name}, std::pair{"type", &out.type}}) {
        const auto value = json::req_str(row, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const auto rfq = json::opt_bool(row, "isRfq");
    const auto status = json::req_str(row, "status");
    if (!rfq.has_value() || !status.has_value()) {
        return std::unexpected(!rfq.has_value() ? rfq.error() : status.error());
    }
    out.rfq = *rfq;
    out.status = *status;
    for (const auto& [key, into] :
         {std::pair{"category", &out.category}, std::pair{"subCategory", &out.sub_category}}) {
        const auto value = json::opt_str(row, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    if (const auto stats = read_stats(row, out); !stats.has_value()) {
        return std::unexpected(stats.error());
    }
    if (const auto config = read_config(row, out); !config.has_value()) {
        return std::unexpected(config.error());
    }
    return out;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == k_max_markets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto market = read_market(row);
            if (!market.has_value()) {
                return std::unexpected(market.error());
            }
            storage.markets[count] = *market;
            ++count;
        }
        return std::span<const WireMarket>{storage.markets.data(), count};
    });
}

} // namespace raikiri::venues::extended
