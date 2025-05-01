// Public-stream frames: topic dispatch, order book, trades, tickers.

#include "wire_detail.hpp"

namespace raikiri::venues::bybit {

namespace {

using detail::guarded;
namespace ondemand = detail::ondemand;

// Bybit topics are "kind.arg[.arg]" — the symbol is always the last segment.
// Resolving the kind once here turns the per-message dispatch that follows
// into an integer compare instead of a chain of string compares at line rate.
TopicInfo classify_topic(std::string_view topic) noexcept {
    TopicInfo info{};
    const auto last_dot = topic.rfind('.');
    if (last_dot != std::string_view::npos) {
        info.symbol = topic.substr(last_dot + 1);
    }
    if (topic.starts_with("orderbook.")) {
        info.kind = TopicKind::Orderbook;
    } else if (topic.starts_with("publicTrade.")) {
        info.kind = TopicKind::PublicTrade;
    } else if (topic.starts_with("tickers.")) {
        info.kind = TopicKind::Tickers;
    } else if (topic == "order") {
        info.kind = TopicKind::Order;
        info.symbol = {};
    } else if (topic == "execution") {
        info.kind = TopicKind::Execution;
        info.symbol = {};
    } else if (topic == "position") {
        info.kind = TopicKind::Position;
        info.symbol = {};
    } else if (topic == "wallet") {
        info.kind = TopicKind::Wallet;
        info.symbol = {};
    } else {
        info.kind = TopicKind::Unknown;
        info.symbol = {};
    }
    return info;
}

// Levels arrive as ["price", "size"] pairs. Anything else is a schema change
// rather than a value we could interpret conservatively.
std::expected<std::size_t, WireError>
read_levels(ondemand::array levels, WireLevel* out, std::size_t capacity) {
    std::size_t count = 0;
    for (auto entry : levels) {
        if (count == capacity) {
            return std::unexpected{WireError::TooManyEntries};
        }
        ondemand::array pair;
        if (entry.get_array().get(pair) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        std::size_t field = 0;
        WireLevel level{};
        for (auto item : pair) {
            ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected{text.error()};
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.size = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected{WireError::MissingField};
        }
        out[count++] = level;
    }
    return count;
}

} // namespace

std::expected<TopicInfo, WireError> parse_topic(std::string_view frame) {
    return guarded([&]() -> std::expected<TopicInfo, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        if (detail::has_field(obj, "topic")) {
            auto topic = detail::req_str(obj, "topic");
            if (!topic.has_value()) {
                return std::unexpected{topic.error()};
            }
            return classify_topic(*topic);
        }
        obj.reset();
        const auto op = detail::opt_str(obj, "op");
        obj.reset();
        const auto ret_msg = detail::opt_str(obj, "ret_msg");
        const auto op_text = op.value_or(std::string_view{});
        const auto msg_text = ret_msg.value_or(std::string_view{});
        if (op_text == "pong" || msg_text == "pong") {
            return TopicInfo{.kind = TopicKind::Pong, .symbol = {}};
        }
        if (!op_text.empty() || detail::has_field(obj, "success")) {
            return TopicInfo{.kind = TopicKind::OpResponse, .symbol = {}};
        }
        return std::unexpected{WireError::UnknownTopic};
    });
}

std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame,
                                                        OrderbookBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderbook, WireError> {
        auto& session = detail::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireOrderbook out{};
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

        auto symbol = detail::req_str(data_obj, "s");
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        out.symbol = *symbol;

        data_obj.reset();
        out.update_id = detail::opt_u64(data_obj, "u").value_or(0);
        data_obj.reset();
        out.seq = detail::opt_u64(data_obj, "seq").value_or(0);

        data_obj.reset();
        std::size_t bid_count = 0;
        if (auto bids = detail::req_array(data_obj, "b"); bids.has_value()) {
            const auto n = read_levels(*bids, storage.bids, k_max_levels);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            bid_count = *n;
        }
        data_obj.reset();
        std::size_t ask_count = 0;
        if (auto asks = detail::req_array(data_obj, "a"); asks.has_value()) {
            const auto n = read_levels(*asks, storage.asks, k_max_levels);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            ask_count = *n;
        }
        out.bids = {storage.bids, bid_count};
        out.asks = {storage.asks, ask_count};
        return out;
    });
}

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
        return out;
    });
}

} // namespace raikiri::venues::bybit
