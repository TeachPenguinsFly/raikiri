#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::binance {

namespace {

using wire::guarded;
using wire::session;

EventKind kind_from_event(std::string_view e) noexcept {
    if (e == "bookTicker") {
        return EventKind::BookTicker;
    }
    // both spellings share one shape; the adapter subscribes to @trade
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
        auto data = wire::opt_raw(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        // a single-stream socket delivers the payload bare
        return data->empty() ? frame : *data;
    });
}

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    auto inner = unwrap(frame);
    if (!inner.has_value()) {
        return std::unexpected(inner.error());
    }
    return parse_event_payload(*inner);
}

std::expected<EventInfo, WireError> parse_event_payload(std::string_view payload) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(payload);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto event = wire::opt_str(*root, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (event->empty()) {
            const auto err = wire::opt_raw(*root, "error");
            if (!err.has_value()) {
                return std::unexpected(err.error());
            }
            if (!err->empty()) {
                info.kind = EventKind::Error;
                return info;
            }
        }
        auto symbol = wire::opt_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        info.symbol = *symbol;
        // spot's bookTicker is the one payload with a symbol and no event type
        if (event->empty()) {
            info.kind = symbol->empty() ? EventKind::SubscribeAck : EventKind::BookTicker;
            return info;
        }
        info.kind = kind_from_event(*event);
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = wire::opt_u64(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto error = wire::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto code = wire::opt_i64(*error, "code");
        const auto msg = wire::opt_str(*error, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg, .id = *id};
    });
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return RestError{};
        }
        const auto code = wire::opt_i64(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (*code >= 0) {
            return RestError{};
        }
        const auto msg = wire::opt_str(*root, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return RestError{.code = *code, .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto time = wire::opt_u64(*root, "serverTime");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        if (*time == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *time;
    });
}

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto key = wire::opt_str(*root, "listenKey");
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        if (key->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return *key;
    });
}

} // namespace raikiri::venues::binance
