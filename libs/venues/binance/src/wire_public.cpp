#include "wire_detail.hpp"

namespace raikiri::venues::binance {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_event(std::string_view e) noexcept {
    if (e == "bookTicker") {
        return EventKind::BookTicker;
    }
    if (e == "aggTrade") {
        return EventKind::AggTrade;
    }
    if (e == "markPriceUpdate") {
        return EventKind::MarkPrice;
    }
    if (e == "depthUpdate") {
        return EventKind::DepthUpdate;
    }
    if (e == "ORDER_TRADE_UPDATE") {
        return EventKind::OrderUpdate;
    }
    if (e == "ACCOUNT_UPDATE") {
        return EventKind::AccountUpdate;
    }
    if (e == "listenKeyExpired") {
        return EventKind::ListenKeyExpired;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<std::string_view, WireError> unwrap(std::string_view frame) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::opt_raw(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        // No wrapper: a single-stream socket delivers the payload bare.
        return data->empty() ? frame : *data;
    });
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    auto inner = unwrap(frame);
    if (!inner.has_value()) {
        return std::unexpected(inner.error());
    }
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(*inner);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto event = detail::opt_str(*root, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (event->empty()) {
            const auto err = detail::opt_raw(*root, "error");
            if (!err.has_value()) {
                return std::unexpected(err.error());
            }
            info.kind = err->empty() ? EventKind::SubscribeAck : EventKind::Error;
            return info;
        }
        info.kind = kind_from_event(*event);
        auto symbol = detail::opt_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        info.symbol = *symbol;
        return info;
    });
}

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view data) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBookTicker out{};
        const auto symbol = detail::req_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        for (const auto& [key, field] : {std::pair{"b", &out.bid_price},
                                         std::pair{"B", &out.bid_qty},
                                         std::pair{"a", &out.ask_price},
                                         std::pair{"A", &out.ask_qty}}) {
            const auto value = detail::req_str(*root, key);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            *field = *value;
        }
        const auto update_id = detail::opt_u64(*root, "u");
        const auto transaction = detail::opt_u64(*root, "T");
        const auto event = detail::opt_u64(*root, "E");
        if (!update_id.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.transaction_ms = *transaction;
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireAggTrade, WireError> parse_agg_trade(std::string_view data) {
    return guarded([&]() -> std::expected<WireAggTrade, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireAggTrade out{};
        const auto symbol = detail::req_str(*root, "s");
        const auto price = detail::req_str(*root, "p");
        const auto qty = detail::req_str(*root, "q");
        if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.price = *price;
        out.qty = *qty;
        const auto agg_id = detail::opt_u64(*root, "a");
        const auto trade_ms = detail::opt_u64(*root, "T");
        const auto maker = detail::opt_bool(*root, "m");
        if (!agg_id.has_value() || !trade_ms.has_value() || !maker.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.agg_id = *agg_id;
        out.trade_ms = *trade_ms;
        out.buyer_is_maker = *maker;
        return out;
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view data) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireMarkPrice out{};
        const auto symbol = detail::req_str(*root, "s");
        const auto mark = detail::req_str(*root, "p");
        if (!symbol.has_value() || !mark.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.mark_price = *mark;
        const auto index = detail::opt_str(*root, "i");
        const auto rate = detail::opt_str(*root, "r");
        if (!index.has_value() || !rate.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.index_price = *index;
        out.funding_rate = *rate;
        const auto next = detail::opt_u64(*root, "T");
        const auto event = detail::opt_u64(*root, "E");
        if (!next.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_ms = *next;
        out.event_ms = *event;
        return out;
    });
}

namespace {

// Each level is a two-element array of strings: ["64488.50","5.560"].
std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        WireLevel level{};
        std::size_t field = 0;
        for (auto element : pair) {
            detail::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data,
                                                             LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthUpdate, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireDepthUpdate out{};
        const auto symbol = detail::req_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        const auto first = detail::opt_u64(*root, "U");
        const auto last = detail::opt_u64(*root, "u");
        const auto prev = detail::opt_u64(*root, "pu");
        const auto event = detail::opt_u64(*root, "E");
        if (!first.has_value() || !last.has_value() || !prev.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.first_id = *first;
        out.last_id = *last;
        out.prev_id = *prev;
        out.event_ms = *event;

        const auto bids = read_levels(*root, "b", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*root, "a", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = detail::opt_u64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto error = detail::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto code = detail::opt_i64(*error, "code");
        const auto msg = detail::opt_str(*error, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg, .id = *id};
    });
}

} // namespace raikiri::venues::binance
