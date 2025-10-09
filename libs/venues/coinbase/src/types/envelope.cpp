#include <raikiri/venues/coinbase/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::coinbase {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_type(std::string_view type) noexcept {
    if (type == "ticker") {
        return EventKind::Ticker;
    }
    if (type == "match" || type == "last_match") {
        return EventKind::Match;
    }
    if (type == "snapshot") {
        return EventKind::Snapshot;
    }
    if (type == "l2update") {
        return EventKind::L2Update;
    }
    if (type == "subscriptions") {
        return EventKind::Subscriptions;
    }
    if (type == "error") {
        return EventKind::Error;
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
        const auto type = detail::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        info.kind = kind_from_type(*type);
        const auto product = detail::opt_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        info.product_id = *product;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "message");
        const auto reason = detail::opt_str(*root, "reason");
        if (!message.has_value() || !reason.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return StreamError{.message = *message, .reason = *reason};
    });
}

std::expected<std::span<const std::string_view>, WireError>
parse_subscriptions(std::string_view frame, AckBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const std::string_view>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "channels");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_ack_channels) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto name = detail::req_str(obj, "name");
            if (!name.has_value()) {
                return std::unexpected(name.error());
            }
            storage.channels[count] = *name;
            ++count;
        }
        return std::span<const std::string_view>{storage.channels.data(), count};
    });
}

std::expected<std::string_view, WireError> parse_server_time(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::req_str(*root, "iso");
    });
}

std::expected<std::string_view, WireError> parse_rest_error(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::req_str(*root, "message");
    });
}

} // namespace raikiri::venues::coinbase
