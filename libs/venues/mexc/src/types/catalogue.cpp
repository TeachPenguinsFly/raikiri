#include <raikiri/venues/mexc/catalogue.hpp>
#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::mexc {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireInstrument>, WireError> parse_instruments(std::string_view body,
                                                                            InstrumentBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireInstrument>, WireError> {
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
            WireInstrument out{};
            const auto symbol = req_scalar(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            // optional so a row without a pair is dropped by the mapper alone
            const auto base_coin = opt_scalar(obj, "baseCoin");
            const auto quote_coin = opt_scalar(obj, "quoteCoin");
            if (!base_coin.has_value() || !quote_coin.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.base_coin = *base_coin;
            out.quote_coin = *quote_coin;
            const auto settle_coin = opt_scalar(obj, "settleCoin");
            if (!settle_coin.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.settle_coin = *settle_coin;
            out.base_coin_name = opt_scalar(obj, "baseCoinName").value_or(std::string_view{});
            const auto contract_size = req_scalar(obj, "contractSize");
            const auto min_vol = req_scalar(obj, "minVol");
            if (!contract_size.has_value() || !min_vol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.contract_size = *contract_size;
            out.min_vol = *min_vol;
            const auto price_scale = opt_int(obj, "priceScale", -1);
            const auto vol_scale = opt_int(obj, "volScale", -1);
            const auto state = opt_int(obj, "state", -1);
            const auto api_allowed = wire::opt_bool(obj, "apiAllowed");
            if (!price_scale.has_value() || !vol_scale.has_value() || !state.has_value() || !api_allowed.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.price_scale = *price_scale;
            out.vol_scale = *vol_scale;
            out.state = *state;
            out.api_allowed = *api_allowed;
            const auto plates = wire::opt_raw(obj, "conceptPlate");
            if (!plates.has_value()) {
                return std::unexpected(plates.error());
            }
            out.concept_plate = *plates;
            storage.instruments[count] = out;
            ++count;
        }
        return std::span<const WireInstrument>{storage.instruments.data(), count};
    });
}

std::expected<std::span<const WireSpotSymbol>, WireError> parse_spot_symbols(std::string_view body,
                                                                             SpotSymbolBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotSymbol>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "symbols");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto element : *array) {
            if (count == k_max_spot_symbols) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireSpotSymbol out{};
            const auto symbol = field(obj, "symbol");
            if (symbol.empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = symbol;
            // `BTCUSDT` has no separator, so these fields state the pair
            out.base_asset = field(obj, "baseAsset");
            out.quote_asset = field(obj, "quoteAsset");
            // `status` is the string "1"
            out.status = field(obj, "status");
            out.base_size_step = field(obj, "baseSizePrecision");
            out.quote_precision = int_field(obj, "quotePrecision");
            obj.reset();
            out.spot_trading_allowed = wire::opt_bool(obj, "isSpotTradingAllowed").value_or(false);
            storage.symbols[count] = out;
            ++count;
        }
        return std::span<const WireSpotSymbol>{storage.symbols.data(), count};
    });
}

} // namespace raikiri::venues::mexc
