#include "wire_detail.hpp"

namespace raikiri::venues::kraken {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_feed(std::string_view feed) noexcept {
    if (feed == "ticker") {
        return EventKind::Ticker;
    }
    if (feed == "ticker_lite") {
        return EventKind::TickerLite;
    }
    if (feed == "trade" || feed == "trade_snapshot") {
        return EventKind::Trade;
    }
    if (feed == "book" || feed == "book_snapshot") {
        return EventKind::Book;
    }
    if (feed == "open_orders_snapshot") {
        return EventKind::OpenOrdersSnapshot;
    }
    if (feed == "open_orders") {
        return EventKind::OpenOrders;
    }
    // Snapshot and delta are one shape here, unlike open_orders: both carry a
    // `fills` array under the same key.
    if (feed == "fills" || feed == "fills_snapshot") {
        return EventKind::Fills;
    }
    return EventKind::Unknown;
}

EventKind kind_from_event(std::string_view event) noexcept {
    if (event == "subscribed" || event == "unsubscribed") {
        return EventKind::Subscribed;
    }
    if (event == "info") {
        return EventKind::Info;
    }
    if (event == "alert" || event == "error") {
        return EventKind::Alert;
    }
    if (event == "challenge") {
        return EventKind::Challenge;
    }
    return EventKind::Unknown;
}

std::expected<WireTrade, WireError> read_trade(detail::ondemand::object& obj) {
    WireTrade out{};
    const auto product = detail::req_str(obj, "product_id");
    const auto uid = detail::opt_str(obj, "uid");
    const auto type = detail::opt_str(obj, "type");
    const auto side = detail::req_str(obj, "side");
    const auto price = detail::req_num(obj, "price");
    const auto qty = detail::req_num(obj, "qty");
    if (!product.has_value() || !uid.has_value() || !type.has_value() || !side.has_value() ||
        !price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.product_id = *product;
    out.uid = *uid;
    out.type = *type;
    out.aggressor_bought = *side == "buy";
    out.price = *price;
    out.qty = *qty;
    const auto time_ms = detail::opt_ms(obj, "time");
    if (!time_ms.has_value()) {
        return std::unexpected(time_ms.error());
    }
    out.time_ms = *time_ms;
    const auto seq = detail::opt_num(obj, "seq");
    if (!seq.has_value()) {
        return std::unexpected(seq.error());
    }
    out.seq = detail::digits_to_u64(*seq);
    return out;
}

std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    const auto price = detail::req_num(obj, "price");
    const auto qty = detail::req_num(obj, "qty");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
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
        const auto event = detail::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (!event->empty()) {
            info.kind = kind_from_event(*event);
            return info; // control frames name product ids in an array, not a field
        }
        const auto feed = detail::req_str(*root, "feed");
        if (!feed.has_value()) {
            return std::unexpected(feed.error());
        }
        info.kind = kind_from_feed(*feed);
        const auto product = detail::opt_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        info.product_id = *product;
        return info;
    });
}

std::expected<StreamAlert, WireError> parse_stream_alert(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamAlert, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "message");
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        return StreamAlert{.message = *message};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireTicker out{};
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        out.product_id = *product;
        const auto bid = detail::opt_num(*root, "bid");
        const auto ask = detail::opt_num(*root, "ask");
        const auto bid_size = detail::opt_num(*root, "bid_size");
        const auto ask_size = detail::opt_num(*root, "ask_size");
        if (!bid.has_value() || !ask.has_value() || !bid_size.has_value() ||
            !ask_size.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.bid = *bid;
        out.ask = *ask;
        out.bid_size = *bid_size;
        out.ask_size = *ask_size;
        for (const auto& [key, field] :
             {std::pair{"markPrice", &out.mark_price},
              std::pair{"index", &out.index},
              std::pair{"last", &out.last},
              std::pair{"relative_funding_rate", &out.relative_funding_rate},
              std::pair{"relative_funding_rate_prediction", &out.relative_funding_rate_prediction},
              std::pair{"openInterest", &out.open_interest},
              // Optional like the rest of this list: opt_num answers an empty
              // view for a field the frame omits, so a ticker_lite push still
              // yields its touch instead of failing the whole parse.
              std::pair{"volume", &out.volume},
              std::pair{"volumeQuote", &out.volume_quote}}) {
            const auto value = detail::opt_num(*root, key);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            *field = *value;
        }
        const auto next_funding = detail::opt_ms(*root, "next_funding_rate_time");
        const auto time_ms = detail::opt_ms(*root, "time");
        const auto suspended = detail::opt_bool(*root, "suspended");
        if (!next_funding.has_value() || !time_ms.has_value() || !suspended.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.next_funding_rate_time_ms = *next_funding;
        out.time_ms = *time_ms;
        out.suspended = *suspended;
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
        const auto feed = detail::req_str(*root, "feed");
        if (!feed.has_value()) {
            return std::unexpected(feed.error());
        }
        if (*feed == "trade") {
            const auto one = read_trade(*root);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[0] = *one;
            return std::span<const WireTrade>{storage.trades.data(), 1};
        }
        auto array = detail::req_array(*root, "trades");
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
            const auto one = read_trade(obj);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.trades[count] = *one;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto feed = detail::req_str(*root, "feed");
        const auto product = detail::req_str(*root, "product_id");
        if (!feed.has_value() || !product.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireBook out{};
        out.product_id = *product;
        out.snapshot = *feed == "book_snapshot";
        // `seq` is the continuity chain and runs contiguous across both.
        const auto timestamp = detail::opt_ms(*root, "timestamp");
        const auto seq = detail::opt_num(*root, "seq");
        if (!timestamp.has_value() || !seq.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.timestamp_ms = *timestamp;
        out.seq = detail::digits_to_u64(*seq);

        if (out.snapshot) {
            const auto bids = read_levels(*root, "bids", storage.bids);
            if (!bids.has_value()) {
                return std::unexpected(bids.error());
            }
            const auto asks = read_levels(*root, "asks", storage.asks);
            if (!asks.has_value()) {
                return std::unexpected(asks.error());
            }
            out.bids = {storage.bids.data(), *bids};
            out.asks = {storage.asks.data(), *asks};
            return out;
        }
        const auto side = detail::req_str(*root, "side");
        const auto price = detail::req_num(*root, "price");
        const auto qty = detail::req_num(*root, "qty");
        if (!side.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const WireLevel level{.price = *price, .qty = *qty};
        if (*side == "buy") {
            storage.bids[0] = level;
            out.bids = {storage.bids.data(), 1};
        } else if (*side == "sell") {
            storage.asks[0] = level;
            out.asks = {storage.asks.data(), 1};
        } else {
            return std::unexpected(WireError::WrongType);
        }
        return out;
    });
}

} // namespace raikiri::venues::kraken
