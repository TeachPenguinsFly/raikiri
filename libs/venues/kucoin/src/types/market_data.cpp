#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<WireTrade, WireError> parse_trade(std::string_view frame, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(*root,
                         [&](detail::ondemand::object& obj) -> std::expected<WireTrade, WireError> {
                             WireTrade out{};
                             out.symbol = *symbol;
                             const auto trade_id = detail::req_str(obj, "tradeId");
                             const auto price = req_decimal(obj, "price");
                             const auto qty = req_decimal(obj, "size");
                             const auto side = detail::req_str(obj, "side");
                             if (!trade_id.has_value() || !price.has_value() || !qty.has_value() ||
                                 !side.has_value()) {
                                 return std::unexpected(WireError::MissingField);
                             }
                             out.trade_id = *trade_id;
                             out.price = *price;
                             out.qty = *qty;
                             out.aggressor_bought = *side == "buy";
                             if (spot) {
                                 // `time` is nanoseconds as a digit STRING here, where the
                                 // futures print puts nanoseconds in a numeric `ts`. There
                                 // is no sequence field on the spot print at all.
                                 const auto ns = opt_u64(obj, "time");
                                 if (!ns.has_value()) {
                                     return std::unexpected(WireError::WrongType);
                                 }
                                 out.ts_ns = *ns;
                                 out.sequence = 0;
                                 return out;
                             }
                             const auto ts = opt_u64(obj, "ts");
                             const auto sequence = opt_i64(obj, "sequence");
                             if (!ts.has_value() || !sequence.has_value()) {
                                 return std::unexpected(WireError::WrongType);
                             }
                             out.ts_ns = *ts;
                             out.sequence = *sequence;
                             return out;
                         });
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                out.symbol = *symbol;
                const auto mark = req_decimal(obj, "markPrice");
                const auto index = opt_decimal(obj, "indexPrice");
                if (!mark.has_value() || !index.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.mark_price = *mark;
                out.index_price = *index;
                const auto ts = opt_u64(obj, "timestamp");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                out.symbol = *symbol;
                const auto rate = req_decimal(obj, "fundingRate");
                if (!rate.has_value()) {
                    return std::unexpected(rate.error());
                }
                out.rate = *rate;
                const auto granularity = opt_u64(obj, "granularity");
                const auto ts = opt_u64(obj, "timestamp");
                if (!granularity.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.granularity_ms = *granularity;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireFundingRate, WireError> parse_funding_current(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                const auto rate = req_decimal(obj, "value");
                const auto predicted = opt_decimal(obj, "predictedValue");
                if (!rate.has_value() || !predicted.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.rate = *rate;
                out.predicted_rate = *predicted;
                const auto granularity = opt_u64(obj, "granularity");
                const auto funding = opt_u64(obj, "fundingTime");
                const auto ts = opt_u64(obj, "timePoint");
                if (!granularity.has_value() || !funding.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.granularity_ms = *granularity;
                out.next_funding_ms = *funding;
                out.ts_ms = *ts;
                return out;
            });
    });
}

} // namespace raikiri::venues::kucoin
