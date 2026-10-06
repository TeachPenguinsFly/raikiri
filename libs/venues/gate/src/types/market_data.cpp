#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/market_data.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::gate {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireTrade, WireError> read_futures_trade(wire::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = num_u64(obj, "id");
    const auto size = req_num(obj, "size");
    // create_time is whole seconds
    const auto ts = num_u64(obj, "create_time_ms");
    const auto price = req_num(obj, "price");
    const auto contract = wire::req_str(obj, "contract");
    if (!trade_id.has_value() || !size.has_value() || !ts.has_value() || !price.has_value() || !contract.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    // the sign of size is the side
    const auto signed_size = split_sign(*size);
    out.trade_id = static_cast<std::int64_t>(*trade_id);
    out.qty = signed_size.magnitude;
    out.aggressor_bought = !signed_size.negative;
    out.ts_ms = *ts;
    out.price = *price;
    out.contract = *contract;
    return out;
}

// spot names the taker side outright; its create_time_ms is a fractional string, truncated
std::expected<WireTrade, WireError> read_spot_trade(wire::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = num_u64(obj, "id");
    const auto amount = req_num(obj, "amount");
    const auto ts = req_num(obj, "create_time_ms");
    const auto price = req_num(obj, "price");
    const auto pair = wire::req_str(obj, "currency_pair");
    const auto side = wire::req_str(obj, "side");
    if (!trade_id.has_value() || !amount.has_value() || !ts.has_value() || !price.has_value() || !pair.has_value() ||
        !side.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    if (*side != "buy" && *side != "sell") {
        return std::unexpected(WireError::WrongType);
    }
    out.trade_id = static_cast<std::int64_t>(*trade_id);
    out.qty = *amount;
    out.aggressor_bought = *side == "buy";
    out.ts_ms = whole_u64(*ts);
    out.price = *price;
    out.contract = *pair;
    return out;
}

} // namespace

std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // futures batches prints in an array; spot sends one object
        if (spot) {
            auto obj = wire::req_object(*root, "result");
            if (!obj.has_value()) {
                return std::unexpected(obj.error());
            }
            const auto one = read_spot_trade(*obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[0] = *one;
            return std::span<const WireTrade>{storage.trades.data(), 1};
        }
        auto array = wire::req_array(*root, "result");
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
            const auto one = read_futures_trade(obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[count] = *one;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireOptionTicker, WireError> parse_option_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireOptionTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOptionTicker out{};
        out.ts_s = whole_u64(field(*root, "time"));
        auto obj = wire::req_object(*root, "result");
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        const auto contract = wire::req_str(*obj, "name");
        if (!contract.has_value()) {
            return std::unexpected(contract.error());
        }
        out.contract = *contract;
        for (const auto& [key, into] : {std::pair{"mark_price", &out.mark_price},
                                        std::pair{"index_price", &out.index_price},
                                        std::pair{"underlying_price", &out.underlying_price},
                                        std::pair{"bid1_price", &out.bid_price},
                                        std::pair{"ask1_price", &out.ask_price},
                                        std::pair{"bid1_size", &out.bid_qty},
                                        std::pair{"ask1_size", &out.ask_qty},
                                        std::pair{"mark_iv", &out.mark_iv},
                                        std::pair{"bid_iv", &out.bid_iv},
                                        std::pair{"ask_iv", &out.ask_iv},
                                        std::pair{"delta", &out.delta},
                                        std::pair{"gamma", &out.gamma},
                                        std::pair{"vega", &out.vega},
                                        std::pair{"theta", &out.theta},
                                        std::pair{"rho", &out.rho}}) {
            *into = field(*obj, key);
        }
        return out;
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = wire::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTicker out{};
            const auto contract = wire::req_str(obj, "contract");
            if (!contract.has_value()) {
                return std::unexpected(contract.error());
            }
            out.contract = *contract;
            const auto mark = opt_num(obj, "mark_price");
            const auto rate = opt_num(obj, "funding_rate");
            const auto indicative = opt_num(obj, "funding_rate_indicative");
            const auto interval = num_u64(obj, "funding_interval");
            const auto next_apply = num_u64(obj, "funding_next_apply");
            const auto index = opt_num(obj, "index_price");
            const auto ts = num_u64(obj, "t");
            if (!mark.has_value() || !rate.has_value() || !indicative.has_value() || !interval.has_value() ||
                !next_apply.has_value() || !index.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.mark_price = *mark;
            out.funding_rate = *rate;
            out.funding_rate_indicative = *indicative;
            out.funding_interval_s = static_cast<std::uint32_t>(*interval);
            out.funding_next_apply_s = *next_apply;
            out.index_price = *index;
            // optional: an untraded contract omits volume, and its funding must survive
            out.volume_24h_base = opt_num(obj, "volume_24h_base").value_or(std::string_view{});
            out.volume_24h_quote = opt_num(obj, "volume_24h_quote").value_or(std::string_view{});
            out.open_interest = opt_num(obj, "total_size").value_or(std::string_view{});
            out.ts_ms = *ts;
            return out;
        }
        return std::unexpected(WireError::MissingField);
    });
}

std::expected<std::span<const WireTicker>, WireError> parse_spot_tickers(std::string_view body,
                                                                         SpotTickerBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto entry : *root) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto pair = wire::req_str(obj, "currency_pair");
            if (!pair.has_value()) {
                return std::unexpected(pair.error());
            }
            WireTicker out{};
            out.contract = *pair;
            const auto base = wire::opt_str(obj, "base_volume");
            const auto quote = wire::opt_str(obj, "quote_volume");
            if (!base.has_value() || !quote.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.volume_24h_base = *base;
            out.volume_24h_quote = *quote;
            storage.rows[count++] = out;
        }
        return std::span<const WireTicker>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::gate
