#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/venues/bitmex/market_data.hpp>

namespace raikiri::venues::bitmex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// Both sides of a quote are optional, but a price without its size is not: the
// size that stayed behind belongs to the old price and pairing them would put a
// stale quantity on a fresh level.
[[nodiscard]] bool side_is_paired(std::string_view price, std::string_view qty) noexcept {
    return price.empty() == qty.empty();
}

} // namespace

std::expected<std::span<const WireQuote>, WireError> parse_quotes(std::string_view frame,
                                                                  QuoteBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireQuote>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_quotes) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireQuote out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto pool = detail::opt_str(obj, "pool");
            const auto ts = detail::opt_str(obj, "timestamp");
            const auto bid_price = opt_num(obj, "bidPrice");
            const auto bid_qty = opt_num(obj, "bidSize");
            const auto ask_price = opt_num(obj, "askPrice");
            const auto ask_qty = opt_num(obj, "askSize");
            if (!symbol.has_value() || !pool.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (!bid_price.has_value() || !bid_qty.has_value() || !ask_price.has_value() ||
                !ask_qty.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            if (!side_is_paired(*bid_price, *bid_qty) || !side_is_paired(*ask_price, *ask_qty)) {
                return std::unexpected(WireError::MissingField);
            }
            if (bid_price->empty() && ask_price->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.pool = *pool;
            out.ts = *ts;
            out.bid_price = *bid_price;
            out.bid_qty = *bid_qty;
            out.ask_price = *ask_price;
            out.ask_qty = *ask_qty;
            storage.quotes[count] = out;
            ++count;
        }
        return std::span<const WireQuote>{storage.quotes.data(), count};
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
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
            WireTrade out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto side = detail::req_str(obj, "side");
            const auto ts = detail::req_str(obj, "timestamp");
            const auto trade_id = detail::opt_str(obj, "trdMatchID");
            const auto price = opt_num(obj, "price");
            const auto qty = opt_num(obj, "size");
            if (!symbol.has_value() || !side.has_value() || !ts.has_value() ||
                !trade_id.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (!price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            if (price->empty() || qty->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "Buy";
            out.ts = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireInstrumentTick, WireError> parse_instrument_tick(std::string_view frame) {
    return guarded([&]() -> std::expected<WireInstrumentTick, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(
            *root,
            [](detail::ondemand::object& obj) -> std::expected<WireInstrumentTick, WireError> {
                WireInstrumentTick out{};
                const auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                const auto ts = detail::opt_str(obj, "timestamp");
                const auto funding_ts = detail::opt_str(obj, "fundingTimestamp");
                const auto interval = detail::opt_str(obj, "fundingInterval");
                const auto mark = opt_num(obj, "markPrice");
                const auto rate = opt_num(obj, "fundingRate");
                const auto indicative = opt_num(obj, "indicativeFundingRate");
                if (!ts.has_value() || !funding_ts.has_value() || !interval.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                if (!mark.has_value() || !rate.has_value() || !indicative.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts = *ts;
                out.funding_ts = *funding_ts;
                out.funding_interval = *interval;
                out.mark_price = *mark;
                out.funding_rate = *rate;
                out.indicative_funding_rate = *indicative;
                // OPTIONAL, unlike the rest: an update frame carries these two
                // only when the day's turnover moved, and refusing the frame
                // over an absent column would drop its funding rate too.
                out.home_notional_24h =
                    opt_num(obj, "homeNotional24h").value_or(std::string_view{});
                out.foreign_notional_24h =
                    opt_num(obj, "foreignNotional24h").value_or(std::string_view{});
                return out;
            });
    });
}

} // namespace raikiri::venues::bitmex
