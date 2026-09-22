#include <raikiri/venues/xt/envelope.hpp>
#include <raikiri/venues/xt/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::xt {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<WireTrade, WireError> parse_trade(std::string_view frame, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [&](wire::ondemand::object& data) -> std::expected<WireTrade, WireError> {
                WireTrade out{};
                const auto symbol = wire::req_str(data, "s");
                const auto price = wire::req_str(data, "p");
                // The two trees name the size differently — "a" on futures,
                // "q" on spot — and say the aggressor differently too.
                const auto qty = wire::req_str(data, spot ? "q" : "a");
                if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.symbol = *symbol;
                out.price = *price;
                out.qty = *qty;
                if (spot) {
                    // `b` says the BUYER WAS THE MAKER, so the aggressor is on
                    // the opposite side — the inverse of the futures tree's
                    // `m`, which names the taker directly. VERIFIED against 53
                    // live prints matched to the concurrent book: 52 agreed, 1
                    // disagreed on a quote that had already moved. Reading the
                    // two trees the same way is how a signed flow series comes
                    // out backwards on one of them.
                    const auto buyer_is_maker = wire::opt_bool(data, "b");
                    if (!buyer_is_maker.has_value()) {
                        return std::unexpected(WireError::WrongType);
                    }
                    out.aggressor_bought = !*buyer_is_maker;
                } else {
                    const auto side = wire::req_str(data, "m");
                    if (!side.has_value()) {
                        return std::unexpected(WireError::MissingField);
                    }
                    if (*side != "BID" && *side != "ASK") {
                        return std::unexpected(WireError::WrongType);
                    }
                    out.aggressor_bought = *side == "BID";
                }
                const auto ts = opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
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
        return with_data(
            *root, [](wire::ondemand::object& data) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                const auto symbol = wire::req_str(data, "s");
                const auto mark = wire::req_str(data, "p");
                if (!symbol.has_value() || !mark.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.symbol = *symbol;
                out.mark_price = *mark;
                const auto ts = opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame, MarketType market) {
    const bool spot = market == MarketType::Spot;
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(
            *root, [&](wire::ondemand::object& data) -> std::expected<WireTicker, WireError> {
                WireTicker out{};
                const auto symbol = wire::req_str(data, "s");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                const auto ts = opt_ms(data, "t");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                // OPTIONAL, both of them: a frame that states neither column
                // is still a well-formed ticker, and refusing it would refuse
                // the whole channel over a market that has not traded.
                out.volume_24h_quote = opt_scalar(data, "v").value_or(std::string_view{});
                if (spot) {
                    // `q` ONLY ON SPOT. The futures tree fills that slot with
                    // `a`, a CONTRACT count, and reading it here would put a
                    // number measured in contracts in a base-volume column.
                    out.volume_24h_base = opt_scalar(data, "q").value_or(std::string_view{});
                } else {
                    out.contracts_24h = opt_scalar(data, "a").value_or(std::string_view{});
                }
                return out;
            });
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        WireFundingRate out{};
        const auto symbol = wire::req_str(*result, "symbol");
        const auto rate = wire::opt_raw(*result, "fundingRate");
        if (!symbol.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.symbol = *symbol;
        out.rate_token = *rate;
        const auto next = opt_ms(*result, "nextCollectionTime");
        const auto interval = req_i64(*result, "collectionInternal");
        if (!next.has_value() || !interval.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_collection_ms = *next;
        out.interval_h = *interval;
        return out;
    });
}

} // namespace raikiri::venues::xt
