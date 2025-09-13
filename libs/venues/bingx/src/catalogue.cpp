#include <raikiri/venues/bingx/catalogue.hpp>
#include <raikiri/venues/bingx/envelope.hpp>

namespace raikiri::venues::bingx {

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
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto price_precision = req_i64(obj, "pricePrecision");
            const auto quantity_precision = req_i64(obj, "quantityPrecision");
            const auto status = detail::opt_i64(obj, "status");
            if (!price_precision.has_value() || !quantity_precision.has_value() ||
                !status.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_precision = *price_precision;
            out.quantity_precision = *quantity_precision;
            out.status = *status;
            for (const auto& [key, field] : {std::pair{"size", &out.size},
                                             std::pair{"tradeMinQuantity", &out.trade_min_quantity},
                                             std::pair{"tradeMinUSDT", &out.trade_min_usdt}}) {
                const auto value = detail::opt_raw(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
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
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto array = detail::req_array(*data, "symbols");
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
            WireSpotSymbol out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            for (const auto& [key, field] : {std::pair{"tickSize", &out.tick_size},
                                             std::pair{"stepSize", &out.step_size},
                                             std::pair{"minNotional", &out.min_notional}}) {
                const auto value = opt_scalar(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto status = req_i64(obj, "status");
            if (!status.has_value()) {
                return std::unexpected(status.error());
            }
            out.status = *status;
            const auto buy = detail::opt_bool(obj, "apiStateBuy");
            const auto sell = detail::opt_bool(obj, "apiStateSell");
            if (!buy.has_value() || !sell.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.api_buy = *buy;
            out.api_sell = *sell;
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::bingx
