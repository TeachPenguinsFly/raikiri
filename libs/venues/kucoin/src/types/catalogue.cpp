#include <raikiri/venues/kucoin/catalogue.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::kucoin {

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
            const auto status = wire::req_str(obj, "status");
            const auto type = wire::req_str(obj, "type");
            if (!symbol.has_value() || !status.has_value() || !type.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.status = *status;
            out.type = *type;
            // optional so a row without a pair costs only that row
            const auto base_currency = wire::opt_str(obj, "baseCurrency");
            const auto quote_currency = wire::opt_str(obj, "quoteCurrency");
            if (!base_currency.has_value() || !quote_currency.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.base_currency = *base_currency;
            out.quote_currency = *quote_currency;
            out.asset_class = wire::opt_str(obj, "assetClass").value_or(std::string_view{});
            const auto inverse = wire::opt_bool(obj, "isInverse");
            if (!inverse.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.is_inverse = *inverse;
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
            // optional: a never-traded contract carries neither field
            for (const auto& [key, field] :
                 {std::pair{"fundingFeeRate", &out.funding_fee_rate},
                  std::pair{"predictedFundingFeeRate", &out.predicted_funding_fee_rate}}) {
                if (const auto value = opt_decimal(obj, key); value.has_value()) {
                    *field = *value;
                }
            }
            if (const auto next = opt_u64(obj, "nextFundingRateDateTime"); next.has_value()) {
                out.next_funding_ms = *next;
            }
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
        auto array = wire::req_array(*root, "data");
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
                const auto trading = wire::opt_bool(obj, "enableTrading");
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

std::expected<std::span<const WireSpotTicker>, WireError>
parse_spot_tickers(std::string_view body, SpotTickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotTicker>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto time = opt_u64(*data, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        auto array = wire::req_array(*data, "ticker");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        const auto count = read_rows<WireSpotTicker>(
            *array,
            storage.tickers,
            [&](auto& obj, WireSpotTicker& row) -> std::expected<void, WireError> {
                const auto symbol = wire::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                row.symbol = *symbol;
                row.ts_ms = *time;
                for (const auto& [key, field] :
                     {std::pair{"vol", &row.vol}, std::pair{"volValue", &row.vol_value}}) {
                    const auto value = opt_decimal(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *field = *value;
                }
                return {};
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireSpotTicker>{storage.tickers.data(), *count};
    });
}

} // namespace raikiri::venues::kucoin
