#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/venues/gate/market_data.hpp>

namespace raikiri::venues::gate {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WireTrade, WireError> read_futures_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = num_u64(obj, "id");
    const auto size = req_num(obj, "size");
    // create_time_ms, not create_time: the latter is whole seconds.
    const auto ts = num_u64(obj, "create_time_ms");
    const auto price = req_num(obj, "price");
    const auto contract = detail::req_str(obj, "contract");
    if (!trade_id.has_value() || !size.has_value() || !ts.has_value() || !price.has_value() ||
        !contract.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    // The SIGN of the size is the side; there is no side field.
    const auto signed_size = split_sign(*size);
    out.trade_id = static_cast<std::int64_t>(*trade_id);
    out.qty = signed_size.magnitude;
    out.aggressor_bought = !signed_size.negative;
    out.ts_ms = *ts;
    out.price = *price;
    out.contract = *contract;
    return out;
}

// Spot renames every field the futures print uses and says the side outright:
// `amount` for size, `currency_pair` for the symbol, and `side` naming the
// TAKER — so no sign to split. Its create_time_ms is a decimal STRING with a
// fractional part ("1787832783276.263"), which whole_u64 truncates at the dot.
std::expected<WireTrade, WireError> read_spot_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto trade_id = num_u64(obj, "id");
    const auto amount = req_num(obj, "amount");
    const auto ts = req_num(obj, "create_time_ms");
    const auto price = req_num(obj, "price");
    const auto pair = detail::req_str(obj, "currency_pair");
    const auto side = detail::req_str(obj, "side");
    if (!trade_id.has_value() || !amount.has_value() || !ts.has_value() || !price.has_value() ||
        !pair.has_value() || !side.has_value()) {
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
        // Futures batch prints into an array; spot sends one object.
        if (spot) {
            auto obj = detail::req_object(*root, "result");
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
        auto array = detail::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
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

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "result");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTicker out{};
            const auto contract = detail::req_str(obj, "contract");
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
            if (!mark.has_value() || !rate.has_value() || !indicative.has_value() ||
                !interval.has_value() || !next_apply.has_value() || !index.has_value() ||
                !ts.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.mark_price = *mark;
            out.funding_rate = *rate;
            out.funding_rate_indicative = *indicative;
            out.funding_interval_s = static_cast<std::uint32_t>(*interval);
            out.funding_next_apply_s = *next_apply;
            out.index_price = *index;
            // OPTIONAL, unlike the rest: a contract that has not traded in the
            // window carries no volume field at all, and refusing the whole
            // frame over it would drop its funding rate too.
            out.volume_24h_base = opt_num(obj, "volume_24h_base").value_or(std::string_view{});
            out.volume_24h_quote = opt_num(obj, "volume_24h_quote").value_or(std::string_view{});
            out.ts_ms = *ts;
            return out;
        }
        return std::unexpected(WireError::MissingField);
    });
}

} // namespace raikiri::venues::gate
