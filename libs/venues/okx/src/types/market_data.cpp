#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/venues/okx/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

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
            const auto inst_id = detail::req_str(obj, "instId");
            const auto trade_id = detail::opt_str(obj, "tradeId");
            const auto price = detail::req_str(obj, "px");
            const auto qty = detail::req_str(obj, "sz");
            const auto side = detail::req_str(obj, "side");
            if (!inst_id.has_value() || !trade_id.has_value() || !price.has_value() ||
                !qty.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.inst_id = *inst_id;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "buy";
            const auto ts = detail::opt_ms(obj, "ts");
            const auto seq = detail::opt_i64(obj, "seqId");
            if (!ts.has_value() || !seq.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.ts_ms = *ts;
            out.seq_id = *seq;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                const auto inst_id = detail::req_str(obj, "instId");
                if (!inst_id.has_value()) {
                    return std::unexpected(inst_id.error());
                }
                out.inst_id = *inst_id;
                const auto rate = detail::opt_str(obj, "fundingRate");
                const auto next_rate = detail::opt_str(obj, "nextFundingRate");
                if (!rate.has_value() || !next_rate.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.funding_rate = *rate;
                out.next_funding_rate = *next_rate;
                const auto funding = detail::opt_ms(obj, "fundingTime");
                const auto next = detail::opt_ms(obj, "nextFundingTime");
                const auto ts = detail::opt_ms(obj, "ts");
                if (!funding.has_value() || !next.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.funding_ms = *funding;
                out.next_funding_ms = *next;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireTicker, WireError> {
                WireTicker out{};
                const auto inst_id = detail::req_str(obj, "instId");
                const auto inst_type = detail::req_str(obj, "instType");
                if (!inst_id.has_value() || !inst_type.has_value()) {
                    // Without instType the two volume columns cannot be told
                    // apart, and guessing puts a contract count on the board.
                    return std::unexpected(WireError::MissingField);
                }
                out.inst_id = *inst_id;
                out.inst_type = *inst_type;
                // OPTIONAL: an instrument that has not traded in the window
                // states no volume, and refusing the frame over it would cost
                // every other fact on it.
                const auto vol = detail::opt_str(obj, "vol24h");
                const auto vol_ccy = detail::opt_str(obj, "volCcy24h");
                const auto last = detail::opt_str(obj, "last");
                if (!vol.has_value() || !vol_ccy.has_value() || !last.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.vol_24h = *vol;
                out.vol_ccy_24h = *vol_ccy;
                out.last = *last;
                const auto ts = detail::opt_ms(obj, "ts");
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
        return with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                const auto inst_id = detail::req_str(obj, "instId");
                const auto mark = detail::req_str(obj, "markPx");
                if (!inst_id.has_value() || !mark.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.inst_id = *inst_id;
                out.mark_price = *mark;
                const auto ts = detail::opt_ms(obj, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return out;
            });
    });
}

} // namespace raikiri::venues::okx
