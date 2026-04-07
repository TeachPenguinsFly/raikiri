#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <charconv>

namespace raikiri::venues::lighter {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

struct Named {
    std::string_view suffix;
    EventKind kind;
};

// By the part after the slash, so `subscribed/` and `update/` share one row.
constexpr std::array<Named, 4> k_kinds{{
    {"order_book", EventKind::OrderBook},
    {"trade", EventKind::Trade},
    {"market_stats", EventKind::MarketStats},
    {"spot_market_stats", EventKind::SpotMarketStats},
}};

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = detail::opt_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (type->empty()) {
            // The refusal is the one frame with no type at all.
            if (detail::has_field(*root, "error")) {
                return EventInfo{.kind = EventKind::Error, .snapshot = false};
            }
            return std::unexpected(WireError::UnknownMessage);
        }
        if (*type == "connected") {
            return EventInfo{.kind = EventKind::Connected, .snapshot = false};
        }
        const auto slash = type->find('/');
        if (slash == std::string_view::npos) {
            return EventInfo{};
        }
        const auto verb = type->substr(0, slash);
        const auto noun = type->substr(slash + 1);
        EventInfo out{};
        out.snapshot = verb == "subscribed";
        if (!out.snapshot && verb != "update") {
            return EventInfo{}; // "unsubscribed/…" and anything new
        }
        for (const auto& named : k_kinds) {
            if (noun == named.suffix) {
                out.kind = named.kind;
                return out;
            }
        }
        return EventInfo{};
    });
}

std::expected<std::uint32_t, WireError> channel_market(std::string_view channel) {
    const auto colon = channel.rfind(':');
    if (colon == std::string_view::npos || colon + 1 == channel.size()) {
        return std::unexpected(WireError::WrongType);
    }
    const auto digits = channel.substr(colon + 1);
    std::uint32_t id = 0;
    const auto [stop, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (ec != std::errc{} || stop != digits.data() + digits.size()) {
        return std::unexpected(WireError::WrongType);
    }
    return id;
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = detail::req_object(*root, "error");
        if (!error.has_value()) {
            return std::unexpected(error.error());
        }
        const auto code = detail::opt_i64(*error, "code");
        const auto message = detail::opt_str(*error, "message");
        if (!code.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .message = *message};
    });
}

} // namespace raikiri::venues::lighter
