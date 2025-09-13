#include <raikiri/venues/bitget/envelope.hpp>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "ticker") {
        return EventKind::Ticker;
    }
    if (channel == "trade") {
        return EventKind::Trade;
    }
    // Capped snapshots and the incremental 500-level feed ship one element.
    if (channel == "books5" || channel == "books15" || channel == "books") {
        return EventKind::Books;
    }
    if (channel == "orders") {
        return EventKind::Orders;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<std::string_view, WireError> arg_inst_id(detail::ondemand::object& root) {
    auto arg = detail::req_object(root, "arg");
    if (!arg.has_value()) {
        return std::unexpected(arg.error());
    }
    return detail::req_str(*arg, "instId");
}

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
            // The login answer must not be read as a subscribe ack: it is the
            // one control frame whose absence means the private socket is
            // connected and deaf.
            info.kind = *event == "error"   ? EventKind::Error
                        : *event == "login" ? EventKind::LoginAck
                                            : EventKind::SubscribeAck;
            return info;
        }
        auto arg = detail::req_object(*root, "arg");
        if (!arg.has_value()) {
            return std::unexpected(arg.error());
        }
        const auto channel = detail::req_str(*arg, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        info.kind = kind_from_channel(*channel);
        const auto inst_id = detail::opt_str(*arg, "instId");
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        info.inst_id = *inst_id;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg};
    });
}

std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body) {
    return guarded([&]() -> std::expected<RestEnvelope, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::req_str(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return RestEnvelope{.code = *code, .msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto ts = detail::opt_ms(*data, "serverTime");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (*ts == 0) {
            return std::unexpected(WireError::MissingField);
        }
        return *ts;
    });
}

} // namespace raikiri::venues::bitget
