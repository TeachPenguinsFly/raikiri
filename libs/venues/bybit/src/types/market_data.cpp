#include <raikiri/venues/bybit/market_data.hpp>

namespace raikiri::venues::bybit {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        auto data = detail::req_array(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }

        std::size_t count = 0;
        for (auto entry : *data) {
            if (count == k_max_trades) {
                return std::unexpected{WireError::TooManyEntries};
            }
            ondemand::object trade_obj;
            if (entry.get_object().get(trade_obj) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            WireTrade trade{};
            auto symbol = detail::req_str(trade_obj, "s");
            if (!symbol.has_value()) {
                return std::unexpected{symbol.error()};
            }
            trade.symbol = *symbol;
            trade_obj.reset();
            trade.price = detail::opt_str(trade_obj, "p").value_or(std::string_view{});
            trade_obj.reset();
            trade.size = detail::opt_str(trade_obj, "v").value_or(std::string_view{});
            trade_obj.reset();
            // "S" is the TAKER's side: "Buy" means the aggressor lifted the offer.
            trade.buyer_aggressor =
                detail::opt_str(trade_obj, "S").value_or(std::string_view{}) == "Buy";
            trade_obj.reset();
            trade.ts_ms = detail::opt_u64(trade_obj, "T").value_or(0);
            trade_obj.reset();
            trade.trade_id = detail::opt_str(trade_obj, "i").value_or(std::string_view{});
            storage.trades[count++] = trade;
        }
        return std::span<const WireTrade>{storage.trades, count};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireTicker out{};
        const auto type = detail::opt_str(obj, "type");
        out.is_snapshot = type.value_or(std::string_view{}) == "snapshot";
        obj.reset();
        out.ts_ms = detail::opt_u64(obj, "ts").value_or(0);

        obj.reset();
        auto data = detail::req_object(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }
        auto data_obj = *data;

        auto symbol = detail::req_str(data_obj, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        out.symbol = *symbol;

        const auto field = [&](std::string_view key) {
            data_obj.reset();
            return detail::opt_str(data_obj, key).value_or(std::string_view{});
        };
        out.mark_price = field("markPrice");
        out.index_price = field("indexPrice");
        out.funding_rate = field("fundingRate");
        out.next_funding_ms = field("nextFundingTime");
        out.bid1_price = field("bid1Price");
        out.bid1_size = field("bid1Size");
        out.ask1_price = field("ask1Price");
        out.ask1_size = field("ask1Size");
        out.volume_24h = field("volume24h");
        out.turnover_24h = field("turnover24h");
        return out;
    });
}

} // namespace raikiri::venues::bybit
