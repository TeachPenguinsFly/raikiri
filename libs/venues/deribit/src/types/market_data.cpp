#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto params = push_params(*root);
        if (!params.has_value()) {
            return std::unexpected(params.error());
        }
        auto array = wire::req_array(*params, "data");
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
            const auto instrument = wire::req_str(obj, "instrument_name");
            const auto trade_id = wire::opt_str(obj, "trade_id");
            const auto direction = wire::req_str(obj, "direction");
            const auto price = req_num(obj, "price");
            const auto qty = req_num(obj, "amount");
            if (!instrument.has_value() || !trade_id.has_value() || !direction.has_value() || !price.has_value() ||
                !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.instrument = *instrument;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *direction == "buy";
            const auto ts = opt_ms(obj, "timestamp");
            const auto seq = wire::opt_i64(obj, "trade_seq");
            if (!ts.has_value() || !seq.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.ts_ms = *ts;
            out.trade_seq = *seq;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        const auto instrument = wire::req_str(*data, "instrument_name");
        const auto mark = req_num(*data, "mark_price");
        if (!instrument.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.instrument = *instrument;
        out.mark_price = *mark;
        // 0.0 on a perpetual is a real rate
        const auto current = opt_num(*data, "current_funding");
        const auto eight_hour = opt_num(*data, "funding_8h");
        const auto ts = opt_ms(*data, "timestamp");
        if (!current.has_value() || !eight_hour.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.current_funding = *current;
        out.funding_8h = *eight_hour;
        out.ts_ms = *ts;

        // read before greeks to follow the frame's field order; stats may be absent
        if (auto stats = wire::req_object(*data, "stats"); stats.has_value()) {
            out.stats_volume = opt_num(*stats, "volume").value_or(std::string_view{});
            out.stats_volume_usd = opt_num(*stats, "volume_usd").value_or(std::string_view{});
        }

        // options only; scalars are read before the nested greeks object
        out.mark_iv = opt_num(*data, "mark_iv").value_or(std::string_view{});
        out.bid_iv = opt_num(*data, "bid_iv").value_or(std::string_view{});
        out.ask_iv = opt_num(*data, "ask_iv").value_or(std::string_view{});
        out.underlying_price = opt_num(*data, "underlying_price").value_or(std::string_view{});
        out.index_price = opt_num(*data, "index_price").value_or(std::string_view{});
        if (auto greeks = wire::req_object(*data, "greeks"); greeks.has_value()) {
            out.delta = opt_num(*greeks, "delta").value_or(std::string_view{});
            out.gamma = opt_num(*greeks, "gamma").value_or(std::string_view{});
            out.vega = opt_num(*greeks, "vega").value_or(std::string_view{});
            out.theta = opt_num(*greeks, "theta").value_or(std::string_view{});
            out.rho = opt_num(*greeks, "rho").value_or(std::string_view{});
        }
        return out;
    });
}

} // namespace raikiri::venues::deribit
