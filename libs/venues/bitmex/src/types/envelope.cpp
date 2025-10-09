#include <raikiri/venues/bitmex/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitmex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

TableKind table_from_name(std::string_view table) noexcept {
    if (table == "quote") {
        return TableKind::Quote;
    }
    if (table == "trade") {
        return TableKind::Trade;
    }
    if (table == "instrument") {
        return TableKind::Instrument;
    }
    if (table == "orderBookL2_25") {
        return TableKind::OrderBook;
    }
    if (table == "order") {
        return TableKind::Order;
    }
    if (table == "execution") {
        return TableKind::Execution;
    }
    if (table == "position") {
        return TableKind::Position;
    }
    return TableKind::Unknown;
}

FrameAction action_from_name(std::string_view action) noexcept {
    if (action == "partial") {
        return FrameAction::Partial;
    }
    if (action == "insert") {
        return FrameAction::Insert;
    }
    if (action == "update") {
        return FrameAction::Update;
    }
    if (action == "delete") {
        return FrameAction::Delete;
    }
    return FrameAction::None;
}

} // namespace

std::expected<FrameInfo, WireError> parse_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<FrameInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto table = detail::opt_str(*root, "table");
        if (!table.has_value()) {
            return std::unexpected(table.error());
        }
        FrameInfo info{};
        if (!table->empty()) {
            const auto action = detail::opt_str(*root, "action");
            if (!action.has_value()) {
                return std::unexpected(action.error());
            }
            info.table = table_from_name(*table);
            info.action = action_from_name(*action);
            return info;
        }
        const auto error = detail::opt_raw(*root, "error");
        const auto success = detail::opt_raw(*root, "success");
        const auto banner = detail::opt_raw(*root, "info");
        if (!error.has_value() || !success.has_value() || !banner.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (!error->empty()) {
            info.table = TableKind::Error;
        } else if (!success->empty()) {
            info.table = TableKind::SubscribeAck;
        } else if (!banner->empty()) {
            info.table = TableKind::Info;
        }
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "error");
        const auto status = detail::opt_i64(*root, "status");
        if (!message.has_value() || !status.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.status = *status, .message = *message};
    });
}

std::expected<SubscribeAck, WireError> parse_subscribe_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<SubscribeAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto topic = detail::req_str(*root, "subscribe");
        const auto success = detail::opt_bool(*root, "success");
        if (!topic.has_value() || !success.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return SubscribeAck{.topic = *topic, .success = *success};
    });
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = detail::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto message = detail::opt_str(*error, "message");
        const auto name = detail::opt_str(*error, "name");
        if (!message.has_value() || !name.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return RestError{.message = *message, .name = *name};
    });
}

std::expected<std::string_view, WireError> opt_num(detail::ondemand::object& obj,
                                                   std::string_view key) {
    const auto raw = detail::opt_raw(obj, key);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (raw->empty() || *raw == "null") {
        return std::string_view{};
    }
    if (raw->front() == '"' || raw->front() == '{' || raw->front() == '[') {
        return std::unexpected(WireError::WrongType);
    }
    return *raw;
}

} // namespace raikiri::venues::bitmex
