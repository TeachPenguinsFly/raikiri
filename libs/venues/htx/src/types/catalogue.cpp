#include <raikiri/venues/htx/catalogue.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                        ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireContract out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto code = detail::req_str(obj, "contract_code");
            const auto size = detail::opt_raw(obj, "contract_size");
            const auto tick = detail::opt_raw(obj, "price_tick");
            const auto delivery = detail::opt_str(obj, "delivery_time");
            const auto status = detail::opt_i64(obj, "contract_status");
            const auto settlement = opt_ms_str(obj, "settlement_date");
            const auto period = detail::opt_str(obj, "settlement_period");
            const auto business = detail::opt_str(obj, "business_type");
            const auto type = detail::opt_str(obj, "contract_type");
            if (!symbol.has_value() || !code.has_value() || !size.has_value() ||
                !tick.has_value() || !delivery.has_value() || !status.has_value() ||
                !settlement.has_value() || !period.has_value() || !business.has_value() ||
                !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.contract_code = *code;
            out.contract_size = *size;
            out.price_tick = *tick;
            out.delivery_time = *delivery;
            out.contract_status = *status;
            out.settlement_ms = *settlement;
            out.settlement_period_h = static_cast<std::int64_t>(detail::digits_to_u64(*period));
            out.business_type = *business;
            out.contract_type = *type;
            storage.contracts[count] = out;
            ++count;
        }
        return std::span<const WireContract>{storage.contracts.data(), count};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_spot_symbols) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto symbol = detail::req_str(obj, "symbol");
            const auto state = detail::opt_str(obj, "state");
            const auto trading = detail::opt_str(obj, "api-trading");
            const auto price_precision = detail::opt_u64(obj, "price-precision");
            const auto amount_precision = detail::opt_u64(obj, "amount-precision");
            const auto min_value = num_of(obj, "min-order-value");
            if (!symbol.has_value() || !state.has_value() || !trading.has_value() ||
                !price_precision.has_value() || !amount_precision.has_value() ||
                !min_value.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            storage.symbols[count] = {
                .symbol = *symbol,
                .state = *state,
                .api_trading = *trading,
                .min_order_value = *min_value,
                .price_precision = static_cast<std::uint32_t>(*price_precision),
                .amount_precision = static_cast<std::uint32_t>(*amount_precision)};
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::htx
