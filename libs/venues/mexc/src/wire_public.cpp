#include "wire_detail.hpp"

namespace raikiri::venues::mexc {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "push.ticker") {
        return EventKind::Ticker;
    }
    if (channel == "push.deal") {
        return EventKind::Trade;
    }
    if (channel == "push.depth.full") {
        return EventKind::DepthFull;
    }
    if (channel == "push.depth") {
        return EventKind::Depth;
    }
    if (channel == "pong") {
        return EventKind::Pong;
    }
    if (channel == "push.personal.order") {
        return EventKind::PersonalOrder;
    }
    if (channel == "push.personal.order.deal") {
        return EventKind::PersonalDeal;
    }
    if (channel == "push.personal.position") {
        return EventKind::PersonalPosition;
    }
    if (channel == "rs.login") {
        return EventKind::LoginAck;
    }
    if (channel == "rs.error") {
        return EventKind::Error;
    }
    if (channel.starts_with("rs.")) {
        return EventKind::SubscribeAck;
    }
    return EventKind::Unknown;
}

// The symbol sits beside `channel` in the envelope, not inside `data`. Reading
// it first keeps the object walk forward on a data frame, where MEXC emits it
// before the payload.
std::expected<std::string_view, WireError> envelope_symbol(detail::ondemand::object& root) {
    return detail::opt_scalar(root, "symbol");
}

// A level is [64150.5,65907,7]: price, size in CONTRACTS, and the resting
// order count. Only the first two are read.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array triple;
    if (const auto ec = entry.get_array().get(triple); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : triple) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto text = detail::scalar_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (field == 0 ? level.price : level.qty) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        const auto channel = detail::req_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        return EventInfo{.kind = kind_from_channel(*channel), .symbol = *symbol};
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto msg = detail::req_scalar(*root, "data");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return StreamError{.msg = *msg};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireTicker out{};
        out.symbol = *symbol;
        const auto last = detail::req_scalar(*data, "lastPrice");
        const auto fair = detail::req_scalar(*data, "fairPrice");
        const auto index = detail::req_scalar(*data, "indexPrice");
        const auto bid = detail::req_scalar(*data, "bid1");
        const auto ask = detail::req_scalar(*data, "ask1");
        if (!last.has_value() || !fair.has_value() || !index.has_value() || !bid.has_value() ||
            !ask.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.last_price = *last;
        out.fair_price = *fair;
        out.index_price = *index;
        out.bid_price = *bid;
        out.ask_price = *ask;
        const auto funding = detail::opt_scalar(*data, "fundingRate");
        const auto ts = detail::opt_ms(*data, "timestamp");
        if (!funding.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.funding_rate = *funding;
        out.ts_ms = *ts;
        return out;
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
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
            out.symbol = *symbol; // per-print objects carry no symbol of their own
            const auto price = detail::req_scalar(obj, "p");
            const auto qty = detail::req_scalar(obj, "v");
            if (!price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.price = *price;
            out.qty = *qty;
            const auto side = detail::opt_i64(obj, "T", 0);
            const auto ts = detail::opt_ms(obj, "t");
            const auto id = detail::opt_scalar(obj, "i");
            if (!side.has_value() || !ts.has_value() || !id.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            // 1 buy, 2 sell.
            if (*side != 1 && *side != 2) {
                return std::unexpected(WireError::WrongType);
            }
            out.aggressor_bought = *side == 1;
            out.ts_ms = *ts;
            out.trade_id = *id;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = envelope_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireDepth out{};
        out.symbol = *symbol;
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        // begin/end are absent on a snapshot; version is on both.
        const auto begin = detail::opt_i64(*data, "begin", 0);
        const auto end = detail::opt_i64(*data, "end", 0);
        const auto version = detail::opt_i64(*data, "version", 0);
        const auto cts = detail::opt_ms(*data, "cts");
        if (!begin.has_value() || !end.has_value() || !version.has_value() || !cts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.begin = *begin;
        out.end = *end;
        out.version = *version;
        out.ts_ms = *cts;
        if (out.ts_ms == 0) {
            const auto ts = detail::opt_ms(*data, "timestamp");
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.ts_ms = *ts;
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        const auto channel = detail::opt_scalar(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        out.snapshot = channel->empty() || *channel == "push.depth.full";
        return out;
    });
}

} // namespace raikiri::venues::mexc
