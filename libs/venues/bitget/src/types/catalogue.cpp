#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::bitget {

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
            if (count == k_max_contracts) {
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
            // optional here so the mapper refuses the row
            for (const auto& [key, target] : {std::pair{"baseCoin", &out.base_coin},
                                              std::pair{"quoteCoin", &out.quote_coin},
                                              std::pair{"symbolType", &out.symbol_type},
                                              std::pair{"symbolStatus", &out.symbol_status},
                                              std::pair{"pricePlace", &out.price_place},
                                              std::pair{"priceEndStep", &out.price_end_step},
                                              std::pair{"sizeMultiplier", &out.size_multiplier},
                                              std::pair{"minTradeNum", &out.min_trade_num},
                                              std::pair{"minTradeUSDT", &out.min_trade_usdt},
                                              std::pair{"fundInterval", &out.fund_interval}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *target = *value;
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
        auto array = wire::req_array(*root, "data");
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
            // optional here so the mapper refuses the row
            for (const auto& [key, target] :
                 {std::pair{"baseCoin", &out.base_coin},
                  std::pair{"quoteCoin", &out.quote_coin},
                  std::pair{"status", &out.status},
                  std::pair{"pricePrecision", &out.price_precision},
                  std::pair{"quantityPrecision", &out.quantity_precision},
                  std::pair{"minTradeAmount", &out.min_trade_amount},
                  std::pair{"minTradeUSDT", &out.min_trade_usdt}}) {
                const auto value = wire::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *target = *value;
            }
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::bitget
