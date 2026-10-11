#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::bitget {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        return read_rows<WireTicker>(
            body, {}, storage.rows, [](wire::ondemand::object& obj, WireTicker& out) -> std::expected<void, WireError> {
                const auto symbol = wire::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.inst_id = *symbol;
                // every column is optional: a halted pair quotes nothing and spot has no funding
                for (const auto& [key, target] : {std::pair{"bidPr", &out.bid_price},
                                                  std::pair{"bidSz", &out.bid_qty},
                                                  std::pair{"askPr", &out.ask_price},
                                                  std::pair{"askSz", &out.ask_qty},
                                                  std::pair{"markPrice", &out.mark_price},
                                                  std::pair{"indexPrice", &out.index_price},
                                                  std::pair{"fundingRate", &out.funding_rate},
                                                  std::pair{"baseVolume", &out.volume_24h_base},
                                                  std::pair{"quoteVolume", &out.volume_24h_quote},
                                                  std::pair{"holdingAmount", &out.open_interest}}) {
                    const auto value = wire::opt_str(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *target = *value;
                }
                const auto ts = wire::opt_ms(obj, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return {};
            });
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        auto array = wire::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTrade out{};
            out.inst_id = *inst_id;
            const auto trade_id = wire::opt_str(obj, "tradeId");
            const auto price = wire::req_str(obj, "price");
            const auto qty = wire::req_str(obj, "size");
            const auto side = wire::req_str(obj, "side");
            if (!trade_id.has_value() || !price.has_value() || !qty.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "buy";
            const auto ts = wire::opt_ms(obj, "ts");
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.ts_ms = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<std::span<const WireFundingRate>, WireError> parse_current_fund_rates(std::string_view body,
                                                                                    FundRateBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFundingRate>, WireError> {
        const auto rows = read_rows<WireFundingRate>(
            body,
            {},
            storage.rows,
            [](wire::ondemand::object& obj, WireFundingRate& out) -> std::expected<void, WireError> {
                const auto symbol = wire::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                for (const auto& [key, target] : {std::pair{"fundingRate", &out.funding_rate},
                                                  std::pair{"fundingRateInterval", &out.interval_hours},
                                                  std::pair{"minFundingRate", &out.min_rate},
                                                  std::pair{"maxFundingRate", &out.max_rate}}) {
                    const auto value = wire::opt_str(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *target = *value;
                }
                const auto next = wire::opt_ms(obj, "nextUpdate");
                if (!next.has_value()) {
                    return std::unexpected(next.error());
                }
                out.next_update_ms = *next;
                return {};
            });
        // an empty list must not become a zero reading
        if (rows.has_value() && rows->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return rows;
    });
}

} // namespace raikiri::venues::bitget
