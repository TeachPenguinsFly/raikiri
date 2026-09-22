#include <raikiri/venues/backpack/catalogue.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::backpack {

namespace {

using wire::guarded;
using wire::session;

// Named apart from wire::opt_u64, which refuses the null every spot row sends.
std::expected<std::uint64_t, WireError> nullable_u64(wire::ondemand::object& obj,
                                                     std::string_view key) {
    wire::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::uint64_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    bool is_null = false;
    if (value.is_null().get(is_null) == simdjson::SUCCESS && is_null) {
        return std::uint64_t{0};
    }
    std::uint64_t out = 0;
    if (const auto num_ec = value.get_uint64().get(out); num_ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(num_ec));
    }
    return out;
}

std::expected<std::string_view, WireError>
nested_str(wire::ondemand::object& filters, std::string_view group, std::string_view key) {
    auto inner = wire::req_object(filters, group);
    if (!inner.has_value()) {
        return std::unexpected(inner.error());
    }
    return wire::opt_str(*inner, key);
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body,
                                                                    MarketBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_markets) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireMarket out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto base = wire::opt_str(obj, "baseSymbol");
            const auto quote = wire::opt_str(obj, "quoteSymbol");
            const auto kind = wire::opt_str(obj, "marketType");
            const auto state = wire::opt_str(obj, "orderBookState");
            const auto rwa = wire::opt_str(obj, "rwaMarketType");
            if (!base.has_value() || !quote.has_value() || !kind.has_value() ||
                !state.has_value() || !rwa.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.rwa_market_type = *rwa;
            out.base_asset = *base;
            out.quote_asset = *quote;
            out.market_type = *kind;
            out.order_book_state = *state;
            const auto interval = nullable_u64(obj, "fundingInterval");
            if (!interval.has_value()) {
                return std::unexpected(interval.error());
            }
            out.funding_interval_ms = *interval;

            auto filters = wire::req_object(obj, "filters");
            if (!filters.has_value()) {
                return std::unexpected(filters.error());
            }
            const auto tick = nested_str(*filters, "price", "tickSize");
            if (!tick.has_value()) {
                return std::unexpected(tick.error());
            }
            out.tick_size = *tick;
            const auto step = nested_str(*filters, "quantity", "stepSize");
            if (!step.has_value()) {
                return std::unexpected(step.error());
            }
            out.step_size = *step;

            storage.markets[count] = out;
            ++count;
        }
        return std::span<const WireMarket>{storage.markets.data(), count};
    });
}

} // namespace raikiri::venues::backpack
