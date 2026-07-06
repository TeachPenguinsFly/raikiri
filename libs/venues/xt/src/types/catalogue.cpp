#include <raikiri/venues/xt/catalogue.hpp>
#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::xt {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError>
parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "result");
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
            WireInstrument out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto product_type = detail::req_str(obj, "productType");
            if (!symbol.has_value() || !product_type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.product_type = *product_type;
            for (const auto& [key, field] : {std::pair{"minStepPrice", &out.tick_size},
                                             std::pair{"contractSize", &out.contract_size},
                                             std::pair{"minNotional", &out.min_notional}}) {
                const auto value = detail::opt_str(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            const auto tags = detail::opt_raw(obj, "tags");
            if (!tags.has_value()) {
                return std::unexpected(tags.error());
            }
            out.tags = *tags;
            const auto state = req_i64(obj, "state");
            const auto precision = req_i64(obj, "quantityPrecision");
            const auto trade_switch = req_bool(obj, "tradeSwitch");
            const auto open_api = req_bool(obj, "isOpenApi");
            if (!state.has_value() || !precision.has_value() || !trade_switch.has_value() ||
                !open_api.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.state = *state;
            out.qty_precision = *precision;
            out.trade_switch = *trade_switch;
            out.open_api = *open_api;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError>
parse_spot_symbols(std::string_view body, SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = detail::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto array = detail::req_array(*result, "symbols");
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
            const auto state = detail::req_str(obj, "state");
            if (!symbol.has_value() || !state.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.state = *state;
            const auto price_precision = req_i64(obj, "pricePrecision");
            const auto qty_precision = req_i64(obj, "quantityPrecision");
            const auto trading = req_bool(obj, "tradingEnabled");
            const auto openapi = req_bool(obj, "openapiEnabled");
            if (!price_precision.has_value() || !qty_precision.has_value() ||
                !trading.has_value() || !openapi.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_precision = *price_precision;
            out.qty_precision = *qty_precision;
            out.trading_enabled = *trading;
            out.openapi_enabled = *openapi;
            // `filters` is a heterogeneous list keyed by a `filter` tag, so the
            // minimum notional is found by walking it rather than by name.
            if (auto filters = detail::req_array(obj, "filters"); filters.has_value()) {
                for (auto filter : *filters) {
                    detail::ondemand::object row;
                    if (const auto ec = filter.get_object().get(row); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    const auto tag = opt_scalar(row, "filter");
                    if (!tag.has_value()) {
                        return std::unexpected(tag.error());
                    }
                    if (*tag != "QUOTE_QTY") {
                        continue;
                    }
                    const auto min = opt_scalar(row, "min");
                    if (!min.has_value()) {
                        return std::unexpected(min.error());
                    }
                    out.min_quote_qty = *min;
                    break;
                }
            }
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::xt
