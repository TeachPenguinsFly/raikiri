#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>

#include <utility>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
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
            const auto status = detail::req_str(obj, "status");
            const auto type = detail::req_str(obj, "type");
            if (!symbol.has_value() || !status.has_value() || !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.status = *status;
            out.type = *type;
            // Optional so a row that names no pair costs that row, not the
            // catalogue; the mapper is what refuses it.
            const auto base_currency = detail::opt_str(obj, "baseCurrency");
            const auto quote_currency = detail::opt_str(obj, "quoteCurrency");
            if (!base_currency.has_value() || !quote_currency.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.base_currency = *base_currency;
            out.quote_currency = *quote_currency;
            for (const auto& [key, field] : {std::pair{"tickSize", &out.tick_size},
                                             std::pair{"lotSize", &out.lot_size},
                                             std::pair{"multiplier", &out.multiplier},
                                             std::pair{"indexPriceTickSize", &out.index_tick_size},
                                             std::pair{"maxOrderQty", &out.max_order_qty}}) {
                const auto value = opt_decimal(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto granularity = opt_u64(obj, "fundingRateGranularity");
            if (!granularity.has_value()) {
                return std::unexpected(granularity.error());
            }
            out.funding_granularity_ms = *granularity;
            // OPTIONAL, unlike everything above: a contract listed but never
            // traded carries neither field, and refusing the row over that
            // would drop the instrument out of the catalogue entirely.
            for (const auto& [key, field] : {std::pair{"volumeOf24h", &out.volume_24h},
                                             std::pair{"turnoverOf24h", &out.turnover_24h}}) {
                if (const auto value = opt_decimal(obj, key); value.has_value()) {
                    *field = *value;
                }
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
        const auto count = read_rows<WireSpotSymbol>(
            *array,
            storage.symbols,
            [](auto& obj, WireSpotSymbol& row) -> std::expected<void, WireError> {
                const auto symbol = req_decimal(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                row.symbol = *symbol;
                for (const auto& [key, field] : {std::pair{"baseIncrement", &row.base_increment},
                                                 std::pair{"priceIncrement", &row.price_increment},
                                                 std::pair{"minFunds", &row.min_funds}}) {
                    const auto value = opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                const auto trading = detail::opt_bool(obj, "enableTrading");
                if (!trading.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                row.enable_trading = *trading;
                return {};
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), *count};
    });
}

} // namespace raikiri::venues::kucoin
