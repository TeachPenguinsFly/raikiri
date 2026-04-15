#include <raikiri/venues/backpack/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::backpack {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_event(std::string_view e) noexcept {
    if (e == "bookTicker") {
        return EventKind::BookTicker;
    }
    if (e == "trade") {
        return EventKind::Trade;
    }
    if (e == "depth") {
        return EventKind::Depth;
    }
    if (e == "markPrice") {
        return EventKind::MarkPrice;
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
        const auto data = detail::opt_raw(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        return data->empty() ? frame : *data;
    });
}

EventKind parse_event_kind(std::string_view payload) {
    const auto kind = guarded([&]() -> std::expected<EventKind, WireError> {
        auto root = session().open_object(payload);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = detail::opt_str(*root, "e");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        if (!event->empty()) {
            return kind_from_event(*event);
        }
        return detail::has_field(*root, "error") ? EventKind::Error : EventKind::Unknown;
    });
    return kind.value_or(EventKind::Unknown);
}

std::expected<RestError, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<RestError, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return RestError{};
        }
        const auto code = detail::opt_str(*root, "code");
        const auto message = detail::opt_str(*root, "message");
        if (!code.has_value() || !message.has_value()) {
            return RestError{};
        }
        return RestError{.code = *code, .message = *message};
    });
}

} // namespace raikiri::venues::backpack
