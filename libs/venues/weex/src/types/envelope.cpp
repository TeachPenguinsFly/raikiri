#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer; a
// venue that reimplements opt_str or req_array is one that will disagree with
// its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

[[nodiscard]] EventKind kind_of(std::string_view event) noexcept {
    if (event == "depth") {
        return EventKind::Depth;
    }
    if (event == "depthSnapshot") {
        return EventKind::DepthSnapshot;
    }
    if (event == "trade") {
        return EventKind::Trade;
    }
    if (event == "tradeSnapshot") {
        return EventKind::TradeSnapshot;
    }
    if (event == "ticker") {
        return EventKind::Ticker;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;

        const auto event = detail::opt_str(obj, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (!event->empty()) {
            EventInfo out{};
            out.kind = kind_of(*event);
            obj.reset();
            const auto symbol = detail::opt_str(obj, "s");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            return out;
        }

        // No "e": a control frame. The verdict comes first because it is the
        // one whose misreading is silent — a refusal read as an acknowledgement
        // looks exactly like a channel with nothing to say.
        obj.reset();
        if (detail::has_field(obj, "result")) {
            obj.reset();
            const auto ok = detail::opt_bool(obj, "result");
            if (!ok.has_value()) {
                return std::unexpected(ok.error());
            }
            return EventInfo{.kind = *ok ? EventKind::SubscribeAck : EventKind::Error,
                             .symbol = {}};
        }

        obj.reset();
        const auto control = detail::opt_str(obj, "event");
        if (!control.has_value()) {
            return std::unexpected(control.error());
        }
        if (*control == "ping") {
            return EventInfo{.kind = EventKind::Ping, .symbol = {}};
        }
        if (*control == "connected") {
            return EventInfo{.kind = EventKind::Connected, .symbol = {}};
        }
        return EventInfo{.kind = EventKind::Unknown, .symbol = {}};
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        StreamError out{};
        const auto msg = detail::opt_str(obj, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        obj.reset();
        const auto id = detail::opt_u64(obj, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;
        return out;
    });
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            // A body that is not an object is premiumIndex or bookTicker, both
            // of which answer with a bare array. Not an error, and not this
            // reader's business.
            return RestError{};
        }
        auto obj = *root;
        const auto code = detail::opt_i64(obj, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        // THE SIGN, not the key. `code` is a legitimate field name inside a
        // success payload, and only a negative one is a refusal.
        if (*code >= 0) {
            return RestError{};
        }
        RestError out{};
        out.code = *code;
        obj.reset();
        const auto msg = detail::opt_str(obj, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        return out;
    });
}

std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::uint64_t, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto obj = *root;
        return detail::opt_u64(obj, "serverTime");
    });
}

} // namespace raikiri::venues::weex
