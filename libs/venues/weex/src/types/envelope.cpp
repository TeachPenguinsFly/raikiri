#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
using wire::session;

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
    if (event == "bookTicker") {
        return EventKind::BookTicker;
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

        const auto event = wire::opt_str(obj, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (!event->empty()) {
            EventInfo out{};
            out.kind = kind_of(*event);
            obj.reset();
            const auto symbol = wire::opt_str(obj, "s");
            if (!symbol.has_value()) {
                return std::unexpected(symbol.error());
            }
            out.symbol = *symbol;
            return out;
        }

        // no "e": a control frame; check the verdict first
        obj.reset();
        if (wire::has_field(obj, "result")) {
            obj.reset();
            const auto ok = wire::opt_bool(obj, "result");
            if (!ok.has_value()) {
                return std::unexpected(ok.error());
            }
            return EventInfo{.kind = *ok ? EventKind::SubscribeAck : EventKind::Error, .symbol = {}};
        }

        obj.reset();
        const auto control = wire::opt_str(obj, "event");
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
        const auto msg = wire::opt_str(obj, "msg");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        out.msg = *msg;
        obj.reset();
        const auto id = wire::opt_u64(obj, "id");
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
            // premiumIndex and bookTicker answer with a bare array
            return RestError{};
        }
        auto obj = *root;
        const auto code = wire::opt_i64(obj, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        // only a negative code is a refusal; code can be a payload field
        if (*code >= 0) {
            return RestError{};
        }
        RestError out{};
        out.code = *code;
        obj.reset();
        const auto msg = wire::opt_str(obj, "msg");
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
        return wire::opt_u64(obj, "serverTime");
    });
}

} // namespace raikiri::venues::weex
