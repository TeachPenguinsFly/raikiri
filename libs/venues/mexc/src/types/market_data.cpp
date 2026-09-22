#include <raikiri/venues/mexc/envelope.hpp>
#include <raikiri/venues/mexc/market_data.hpp>
#include <raikiri/wire/json.hpp>
#include <raikiri/wire/protobuf.hpp>

namespace raikiri::venues::mexc {

namespace {

namespace pb = raikiri::wire::pb;

using wire::guarded;
using wire::session;

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        out.symbol = *symbol;
        const auto last = req_scalar(*data, "lastPrice");
        const auto fair = req_scalar(*data, "fairPrice");
        const auto index = req_scalar(*data, "indexPrice");
        const auto bid = req_scalar(*data, "bid1");
        const auto ask = req_scalar(*data, "ask1");
        if (!last.has_value() || !fair.has_value() || !index.has_value() || !bid.has_value() ||
            !ask.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.last_price = *last;
        out.fair_price = *fair;
        out.index_price = *index;
        out.bid_price = *bid;
        out.ask_price = *ask;
        const auto funding = opt_scalar(*data, "fundingRate");
        const auto ts = scalar_ms(*data, "timestamp");
        if (!funding.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.funding_rate = *funding;
        out.ts_ms = *ts;
        // OPTIONAL, unlike the prices above: a frame that omits the turnover
        // must still yield its funding rate, and this channel is the only
        // place MEXC states one.
        out.volume_24h_quote = opt_scalar(*data, "amount24").value_or(std::string_view{});
        out.volume_24h_contracts = opt_scalar(*data, "volume24").value_or(std::string_view{});
        return out;
    });
}

std::expected<std::span<const WireSpotTicker>, WireError>
parse_spot_tickers(std::string_view body, SpotTickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireSpotTicker>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireSpotTicker out{};
            const auto symbol = req_scalar(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            out.volume_base = opt_scalar(obj, "volume").value_or(std::string_view{});
            out.volume_quote = opt_scalar(obj, "quoteVolume").value_or(std::string_view{});
            out.close_ms = scalar_ms(obj, "closeTime").value_or(0);
            storage.rows[count++] = out;
        }
        return std::span<const WireSpotTicker>{storage.rows.data(), count};
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
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
            out.symbol = *symbol; // per-print objects carry no symbol of their own
            const auto price = req_scalar(obj, "p");
            const auto qty = req_scalar(obj, "v");
            if (!price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.price = *price;
            out.qty = *qty;
            const auto side = opt_int(obj, "T", 0);
            const auto ts = scalar_ms(obj, "t");
            const auto id = opt_scalar(obj, "i");
            if (!side.has_value() || !ts.has_value() || !id.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            // 1 buy, 2 sell.
            if (*side != 1 && *side != 2) {
                return std::unexpected(WireError::WrongType);
            }
            out.aggressor_bought = *side == 1;
            out.ts_ms = *ts;
            out.trade_id = *id;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<std::span<const WireFundingRate>, WireError>
parse_funding_rates(std::string_view body, FundingRateBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireFundingRate>, WireError> {
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
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireFundingRate out{};
            const auto symbol = req_scalar(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            // A row that states no cycle is kept with 0 rather than dropped:
            // "this contract's interval is unknown" is a fact the catalogue
            // must carry, and the mapper is what refuses to guess one.
            const auto cycle = opt_int(obj, "collectCycle", 0);
            const auto max_rate = opt_scalar(obj, "maxFundingRate");
            const auto min_rate = opt_scalar(obj, "minFundingRate");
            if (!cycle.has_value() || !max_rate.has_value() || !min_rate.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.collect_cycle_h = *cycle;
            out.max_rate = *max_rate;
            out.min_rate = *min_rate;
            storage.rows[count++] = out;
        }
        return std::span<const WireFundingRate>{storage.rows.data(), count};
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireFundingRate out{};
        const auto symbol = req_scalar(*data, "symbol");
        const auto rate = req_scalar(*data, "fundingRate");
        if (!symbol.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.funding_rate = *rate;
        const auto cycle = opt_int(*data, "collectCycle", 0);
        const auto next = scalar_ms(*data, "nextSettleTime");
        const auto ts = scalar_ms(*data, "timestamp");
        const auto fair = opt_scalar(*data, "fairPrice");
        if (!cycle.has_value() || !next.has_value() || !ts.has_value() || !fair.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.collect_cycle_h = *cycle;
        out.next_settle_ms = *next;
        out.ts_ms = *ts;
        out.fair_price = *fair;
        return out;
    });
}

std::expected<std::span<const StreamTrade>, WireError>
parse_stream_trades(std::string_view payload, StreamTradeBuffer& storage) {
    std::size_t count = 0;
    pb::Reader reader{payload};
    while (const auto entry = reader.next()) {
        if (entry->number != 1 || entry->type != pb::WireType::Bytes) {
            continue;
        }
        if (count == storage.trades.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        const auto price = pb::bytes_field(entry->bytes, 1);
        const auto qty = pb::bytes_field(entry->bytes, 2);
        const auto trade_type = pb::varint_field(entry->bytes, 3);
        const auto ts = pb::varint_field(entry->bytes, 4);
        const auto id = pb::bytes_field(entry->bytes, 5);
        if (!price.has_value() || !qty.has_value() || !trade_type.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        storage.trades[count] = StreamTrade{.trade_id = id.value_or(std::string_view{}),
                                            .price = *price,
                                            .qty = *qty,
                                            .aggressor_bought = *trade_type == 1,
                                            .ts_ms = *ts};
        ++count;
    }
    if (count == 0) {
        return std::unexpected(WireError::MissingField);
    }
    return std::span<const StreamTrade>{storage.trades.data(), count};
}

} // namespace raikiri::venues::mexc
