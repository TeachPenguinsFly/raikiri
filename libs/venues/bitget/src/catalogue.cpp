#include <raikiri/venues/bitget/catalogue.hpp>
#include <raikiri/venues/bitget/envelope.hpp>

#include <utility>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
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
            if (count == k_max_contracts) {
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
            // Optional here so the mapper is what refuses a row.
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
                const auto value = detail::opt_str(obj, key);
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
            WireSpotSymbol out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            // Optional here so the mapper is what refuses a row.
            for (const auto& [key, target] :
                 {std::pair{"baseCoin", &out.base_coin},
                  std::pair{"quoteCoin", &out.quote_coin},
                  std::pair{"status", &out.status},
                  std::pair{"pricePrecision", &out.price_precision},
                  std::pair{"quantityPrecision", &out.quantity_precision},
                  std::pair{"minTradeAmount", &out.min_trade_amount},
                  std::pair{"minTradeUSDT", &out.min_trade_usdt}}) {
                const auto value = detail::opt_str(obj, key);
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
