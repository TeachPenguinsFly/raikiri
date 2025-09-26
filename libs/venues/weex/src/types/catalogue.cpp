#include <raikiri/venues/weex/catalogue.hpp>

namespace raikiri::venues::weex {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_exchange_info(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        auto array = detail::req_array(obj, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object item;
            if (const auto ec = entry.get_object().get(item); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireInstrument row{};
            for (const auto& [key, field] : {std::pair{"symbol", &row.symbol},
                                             std::pair{"baseAsset", &row.base_asset},
                                             std::pair{"quoteAsset", &row.quote_asset},
                                             std::pair{"contractType", &row.contract_type}}) {
                item.reset();
                const auto value = detail::opt_str(item, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            item.reset();
            // RAW, not a string: `contractVal` is a JSON number, written as an
            // integer on 573 rows and as a float on 449 in the same body, so
            // the literal is taken as text and parsed once by the mapper.
            const auto contract_val = detail::opt_raw(item, "contractVal");
            if (!contract_val.has_value()) {
                return std::unexpected(contract_val.error());
            }
            row.contract_val = *contract_val;
            item.reset();
            const auto precision = detail::opt_u64(item, "pricePrecision");
            if (!precision.has_value()) {
                return std::unexpected(precision.error());
            }
            row.price_precision = *precision;

            // LAST, because reading a nested array consumes it: the settlement
            // timetable is counted and never inspected, so nothing after it
            // needs the object rewound again.
            item.reset();
            auto delivery = detail::req_array(item, "delivery");
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

} // namespace raikiri::venues::weex
