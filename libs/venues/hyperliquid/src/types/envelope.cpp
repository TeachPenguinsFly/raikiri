#include <raikiri/venues/hyperliquid/envelope.hpp>

namespace raikiri::venues::hyperliquid {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bbo") {
        return EventKind::Bbo;
    }
    if (channel == "trades") {
        return EventKind::Trade;
    }
    if (channel == "l2Book") {
        return EventKind::Book;
    }
    if (channel == "activeAssetCtx") {
        return EventKind::AssetCtx;
    }
    if (channel == "subscriptionResponse") {
        return EventKind::SubscribeAck;
    }
    if (channel == "pong") {
        return EventKind::Pong;
    }
    if (channel == "error") {
        return EventKind::Error;
    }
    return EventKind::Unknown;
}

} // namespace

std::expected<EventKind, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventKind, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = detail::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        return kind_from_channel(*channel);
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // `data` is prose, not an object: there is no code to read.
        const auto msg = detail::req_str(*root, "data");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return StreamError{.msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> opt_number_ms(detail::ondemand::object& obj,
                                                      std::string_view key) {
    const auto text = detail::opt_raw(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return detail::digits_to_u64(*text);
}

} // namespace raikiri::venues::hyperliquid
