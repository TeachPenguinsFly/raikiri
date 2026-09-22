#include <raikiri/venues/bingx/catalogue.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bingx {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireContract>, WireError> parse_contracts(std::string_view body,
                                                                        ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_instruments) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireContract out{};
            const auto symbol = wire::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            const auto price_precision = req_i64(obj, "pricePrecision");
            const auto quantity_precision = req_i64(obj, "quantityPrecision");
            const auto status = wire::opt_i64(obj, "status");
            if (!price_precision.has_value() || !quantity_precision.has_value() ||
                !status.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_precision = *price_precision;
            out.quantity_precision = *quantity_precision;
            out.status = *status;
            // ABSENT READS AS OPEN. BingX spells these as the STRINGS "true"
            // and "false" on this tree — opt_bool would refuse them — so they
            // are read as text, and a row that states neither is left tradable
            // rather than silently locked out by a field that moved.
            for (const auto& [key, field] :
                 {std::pair{"apiStateOpen", &out.api_open},
                  std::pair{"apiStateClose", &out.api_close}}) {
                obj.reset();
                const auto text = wire::opt_str(obj, key).value_or(std::string_view{});
                *field = text != "false";
            }
            obj.reset();
            for (const auto& [key, field] : {std::pair{"size", &out.size},
                                             std::pair{"tradeMinQuantity", &out.trade_min_quantity},
                                             std::pair{"tradeMinUSDT", &out.trade_min_usdt}}) {
                const auto value = wire::opt_raw(obj, key);
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
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto array = wire::req_array(*data, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_spot_symbols) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireSpotSymbol out{};
            const auto symbol = wire::req_str(obj, "symbol");
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
            const auto buy = wire::opt_bool(obj, "apiStateBuy");
            const auto sell = wire::opt_bool(obj, "apiStateSell");
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
