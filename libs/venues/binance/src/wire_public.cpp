#include "wire_detail.hpp"

namespace raikiri::venues::binance {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_event(std::string_view e) noexcept {
    if (e == "bookTicker") {
        return EventKind::BookTicker;
    }
    // Both spellings, one shape. The adapter asks for @trade — the aggregate
    // stream is acknowledged on this venue and then never sends a frame, while
    // @trade floods — but a frame from either is readable here, and @trade is
    // strictly the finer of the two: one print per fill instead of one per
    // run of fills at a price.
    if (e == "trade" || e == "aggTrade") {
        return EventKind::Trade;
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

std::expected<WirePrint, WireError> parse_trade(std::string_view data) {
    return guarded([&]() -> std::expected<WirePrint, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WirePrint out{};
        const auto symbol = detail::req_str(*root, "s");
        const auto price = detail::req_str(*root, "p");
        const auto qty = detail::req_str(*root, "q");
        if (!symbol.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.price = *price;
        out.qty = *qty;
        // The id is "a" on the aggregate stream and "t" on the per-trade one.
        // opt_u64 answers 0 for whichever is absent, so this takes the one
        // that arrived without needing to know which channel it came from.
        const auto aggregate_id = detail::opt_u64(*root, "a");
        const auto single_id = detail::opt_u64(*root, "t");
        const auto trade_ms = detail::opt_u64(*root, "T");
        const auto maker = detail::opt_bool(*root, "m");
        if (!aggregate_id.has_value() || !single_id.has_value() || !trade_ms.has_value() ||
            !maker.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.trade_id = *aggregate_id != 0 ? *aggregate_id : *single_id;
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

std::expected<std::span<const WireMarkPrice>, WireError>
parse_premium_index(std::string_view body, PremiumBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireMarkPrice>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_premium_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireMarkPrice out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.mark_price = detail::opt_str(obj, "markPrice").value_or(std::string_view{});
            out.index_price = detail::opt_str(obj, "indexPrice").value_or(std::string_view{});
            out.funding_rate = detail::opt_str(obj, "lastFundingRate").value_or(std::string_view{});
            out.next_funding_ms = detail::opt_u64(obj, "nextFundingTime").value_or(0);
            out.event_ms = detail::opt_u64(obj, "time").value_or(0);
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireMarkPrice>{storage.rows.data(), count};
    });
}

std::expected<std::span<const WireTicker24h>, WireError>
parse_ticker_24h(std::string_view body, Ticker24hBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTicker24h>, WireError> {
        auto array = session().open_array(body);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_ticker_rows) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTicker24h out{};
            const auto symbol = detail::req_str(obj, "symbol");
            if (!symbol.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.volume = detail::opt_str(obj, "volume").value_or(std::string_view{});
            out.quote_volume = detail::opt_str(obj, "quoteVolume").value_or(std::string_view{});
            out.close_ms = detail::opt_u64(obj, "closeTime").value_or(0);
            storage.rows[count] = out;
            ++count;
        }
        return std::span<const WireTicker24h>{storage.rows.data(), count};
    });
}

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

        const auto bids = detail::read_levels(*root, "b", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = detail::read_levels(*root, "a", storage.asks);
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
