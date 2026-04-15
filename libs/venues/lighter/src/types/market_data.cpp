#include <raikiri/venues/lighter/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <utility>

namespace raikiri::venues::lighter {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WirePrint, WireError> read_print(detail::ondemand::object& obj) {
    WirePrint out{};
    const auto price = detail::req_str(obj, "price");
    const auto size = detail::req_str(obj, "size");
    if (!price.has_value() || !size.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.price = *price;
    out.size = *size;
    const auto market = detail::opt_u64(obj, "market_id");
    const auto id = detail::opt_u64(obj, "trade_id");
    const auto ts = detail::opt_u64(obj, "timestamp");
    const auto maker_ask = detail::opt_bool(obj, "is_maker_ask");
    if (!market.has_value() || !id.has_value() || !ts.has_value() || !maker_ask.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.market_id = static_cast<std::uint32_t>(*market);
    out.trade_id = *id;
    out.ts_ms = *ts;
    out.maker_is_ask = *maker_ask;
    return out;
}

std::expected<std::size_t, WireError> read_prints(detail::ondemand::object& root,
                                                  std::string_view key,
                                                  PrintBuffer& storage,
                                                  std::size_t count) {
    auto array = detail::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        if (count == k_max_prints) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto print = read_print(obj);
        if (!print.has_value()) {
            return std::unexpected(print.error());
        }
        storage.prints[count] = *print;
        ++count;
    }
    return count;
}

std::expected<WireMarketStats, WireError> read_stats(detail::ondemand::object& obj) {
    WireMarketStats out{};
    const auto market = detail::opt_u64(obj, "market_id");
    if (!market.has_value()) {
        return std::unexpected(market.error());
    }
    out.market_id = static_cast<std::uint32_t>(*market);
    const std::array<std::pair<std::string_view, std::string_view*>, 3> strings{{
        {"mark_price", &out.mark_price},
        {"current_funding_rate", &out.current_funding_rate},
        {"funding_rate", &out.last_funding_rate},
    }};
    for (const auto& [key, into] : strings) {
        const auto value = detail::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *into = *value;
    }
    const auto funding_ms = detail::opt_u64(obj, "funding_timestamp");
    const auto base = detail::opt_raw(obj, "daily_base_token_volume");
    const auto quote = detail::opt_raw(obj, "daily_quote_token_volume");
    if (!funding_ms.has_value() || !base.has_value() || !quote.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.funding_ms = *funding_ms;
    out.base_volume = *base;
    out.quote_volume = *quote;
    return out;
}

} // namespace

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame,
                                                                  PrintBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto trades = read_prints(*root, "trades", storage, 0);
        if (!trades.has_value()) {
            return std::unexpected(trades.error());
        }
        const auto all = read_prints(*root, "liquidation_trades", storage, *trades);
        if (!all.has_value()) {
            return std::unexpected(all.error());
        }
        return std::span<const WirePrint>{storage.prints.data(), *all};
    });
}

std::expected<WireStatsFrame, WireError> parse_market_stats(std::string_view frame,
                                                            StatsBuffer& storage) {
    return guarded([&]() -> std::expected<WireStatsFrame, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireStatsFrame out{};
        const auto channel = detail::req_str(*root, "channel");
        const auto ts = detail::opt_u64(*root, "timestamp");
        if (!channel.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.ts_ms = *ts;
        const bool all = channel->ends_with(":all");
        const std::string_view key =
            channel->starts_with("spot_") ? "spot_market_stats" : "market_stats";
        auto body = detail::req_object(*root, key);
        if (!body.has_value()) {
            return std::unexpected(body.error());
        }
        if (!all) {
            const auto row = read_stats(*body);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            storage.rows[0] = *row;
            out.rows = {storage.rows.data(), 1};
            return out;
        }
        std::size_t count = 0;
        for (auto entry : *body) {
            if (count == k_max_stats) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::field field;
            if (const auto ec = std::move(entry).get(field); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            detail::ondemand::object obj;
            if (const auto ec = field.value().get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto row = read_stats(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            storage.rows[count] = *row;
            ++count;
        }
        out.rows = {storage.rows.data(), count};
        return out;
    });
}

} // namespace raikiri::venues::lighter
