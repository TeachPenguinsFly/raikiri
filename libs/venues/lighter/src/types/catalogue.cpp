#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::lighter {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireMarket, WireError> read_market(wire::ondemand::object& obj) {
    WireMarket out{};
    const auto symbol = wire::req_str(obj, "symbol");
    const auto type = wire::req_str(obj, "market_type");
    const auto status = wire::req_str(obj, "status");
    if (!symbol.has_value() || !type.has_value() || !status.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = *symbol;
    out.market_type = *type;
    out.status = *status;
    // A zero read as "no decimals" would be a tick of 1.
    if (!wire::has_field(obj, "market_id") || !wire::has_field(obj, "size_decimals") ||
        !wire::has_field(obj, "price_decimals")) {
        return std::unexpected(WireError::MissingField);
    }
    const auto id = wire::opt_u64(obj, "market_id");
    const auto size = wire::opt_u64(obj, "size_decimals");
    const auto price = wire::opt_u64(obj, "price_decimals");
    if (!id.has_value() || !size.has_value() || !price.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.market_id = static_cast<std::uint32_t>(*id);
    out.size_decimals = static_cast<std::uint32_t>(*size);
    out.price_decimals = static_cast<std::uint32_t>(*price);
    const auto min_base = wire::opt_str(obj, "min_base_amount");
    const auto min_quote = wire::opt_str(obj, "min_quote_amount");
    const auto clamp = wire::opt_str(obj, "funding_clamp_big");
    if (!min_base.has_value() || !min_quote.has_value() || !clamp.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.min_base_amount = *min_base;
    out.min_quote_amount = *min_quote;
    out.funding_clamp_big = *clamp;
    // a hint for the asset class only; a bad one costs the row its class, not the catalogue
    out.strategy_index = static_cast<std::uint32_t>(wire::opt_u64(obj, "strategy_index").value_or(0));
    out.funding_premium_multiplier = static_cast<std::uint32_t>(
        wire::opt_u64(obj, "funding_premium_multiplier").value_or(0));
    return out;
}

std::expected<std::size_t, WireError>
read_array(wire::ondemand::object& root, std::string_view key, MarketBuffer& storage, std::size_t count) {
    auto rows = wire::req_array(root, key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    for (auto entry : *rows) {
        if (count == k_max_markets) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto market = read_market(obj);
        if (!market.has_value()) {
            return std::unexpected(market.error());
        }
        storage.markets[count] = *market;
        ++count;
    }
    return count;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_order_book_details(std::string_view body,
                                                                               MarketBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto perps = read_array(*root, "order_book_details", storage, 0);
        if (!perps.has_value()) {
            return std::unexpected(perps.error());
        }
        const auto all = read_array(*root, "spot_order_book_details", storage, *perps);
        if (!all.has_value()) {
            return std::unexpected(all.error());
        }
        return std::span<const WireMarket>{storage.markets.data(), *all};
    });
}

} // namespace raikiri::venues::lighter
