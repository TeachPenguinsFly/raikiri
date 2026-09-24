#include <raikiri/venues/weex/catalogue.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        auto array = wire::req_array(obj, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object item;
            if (const auto ec = entry.get_object().get(item); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireInstrument row{};
            for (const auto& [key, field] : {std::pair{"symbol", &row.symbol},
                                             std::pair{"baseAsset", &row.base_asset},
                                             std::pair{"quoteAsset", &row.quote_asset},
                                             std::pair{"contractType", &row.contract_type},
                                             std::pair{"underlyingType", &row.underlying_type}}) {
                item.reset();
                const auto value = wire::opt_str(item, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            item.reset();
            // raw: contractVal is written as an int or a float
            const auto contract_val = wire::opt_raw(item, "contractVal");
            if (!contract_val.has_value()) {
                return std::unexpected(contract_val.error());
            }
            row.contract_val = *contract_val;
            item.reset();
            const auto precision = wire::opt_u64(item, "pricePrecision");
            if (!precision.has_value()) {
                return std::unexpected(precision.error());
            }
            row.price_precision = *precision;

            // last, because reading the nested array consumes it
            item.reset();
            auto delivery = wire::req_array(item, "delivery");
            if (!delivery.has_value()) {
                return std::unexpected(delivery.error());
            }
            std::uint32_t settlements = 0;
            for ([[maybe_unused]] auto slot : *delivery) {
                ++settlements;
            }
            row.settlements_per_day = settlements;

            storage.instruments[count] = row;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<std::span<const std::string_view>, WireError>
parse_api_trading_symbols(std::string_view body, TradableBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            const auto name = wire::str_of(entry.value());
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            storage.symbols[count++] = *name;
        }
        return std::span<const std::string_view>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::weex
