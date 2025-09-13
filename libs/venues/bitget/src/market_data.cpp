#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/market_data.hpp>

#include <utility>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        return with_first_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireTicker, WireError> {
                WireTicker out{};
                out.inst_id = *inst_id;
                const auto bid = detail::req_str(obj, "bidPr");
                const auto bid_qty = detail::req_str(obj, "bidSz");
                const auto ask = detail::req_str(obj, "askPr");
                const auto ask_qty = detail::req_str(obj, "askSz");
                if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
                    !ask_qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.bid_price = *bid;
                out.bid_qty = *bid_qty;
                out.ask_price = *ask;
                out.ask_qty = *ask_qty;
                // Optional: no funding rate must not cost the BBO with it.
                const auto last = detail::opt_str(obj, "lastPr");
                const auto mark = detail::opt_str(obj, "markPrice");
                const auto index = detail::opt_str(obj, "indexPrice");
                const auto rate = detail::opt_str(obj, "fundingRate");
                if (!last.has_value() || !mark.has_value() || !index.has_value() ||
                    !rate.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.last_price = *last;
                out.mark_price = *mark;
                out.index_price = *index;
                out.funding_rate = *rate;
                // Optional for the same reason, one step further: an
                // instrument that has not traded in the window carries no
                // volume column, and refusing the frame over it would drop
                // the funding rate riding beside it.
                const auto base_volume = detail::opt_str(obj, "baseVolume");
                const auto quote_volume = detail::opt_str(obj, "quoteVolume");
                if (!base_volume.has_value() || !quote_volume.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.volume_24h_base = *base_volume;
                out.volume_24h_quote = *quote_volume;
                const auto next = detail::opt_ms(obj, "nextFundingTime");
                const auto ts = detail::opt_ms(obj, "ts");
                if (!next.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.next_funding_ms = *next;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
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
            out.inst_id = *inst_id;
            const auto trade_id = detail::opt_str(obj, "tradeId");
            const auto price = detail::req_str(obj, "price");
            const auto qty = detail::req_str(obj, "size");
            const auto side = detail::req_str(obj, "side");
            if (!trade_id.has_value() || !price.has_value() || !qty.has_value() ||
                !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "buy";
            const auto ts = detail::opt_ms(obj, "ts");
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

std::expected<WireFundingRate, WireError> parse_current_fund_rate(std::string_view body) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                const auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                for (const auto& [key, target] :
                     {std::pair{"fundingRate", &out.funding_rate},
                      std::pair{"fundingRateInterval", &out.interval_hours},
                      std::pair{"minFundingRate", &out.min_rate},
                      std::pair{"maxFundingRate", &out.max_rate}}) {
                    const auto value = detail::opt_str(obj, key);
                    if (!value.has_value()) {
                        return std::unexpected(value.error());
                    }
                    *target = *value;
                }
                const auto next = detail::opt_ms(obj, "nextUpdate");
                if (!next.has_value()) {
                    return std::unexpected(next.error());
                }
                out.next_update_ms = *next;
                return out;
            });
    });
}

} // namespace raikiri::venues::bitget
