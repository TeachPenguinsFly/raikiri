#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

using wire::guarded;
using wire::session;

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
    // a spot coin subscribed on activeAssetCtx is answered on activeSpotAssetCtx
    if (channel == "activeAssetCtx" || channel == "activeSpotAssetCtx") {
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
        const auto channel = wire::req_str(*root, "channel");
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
        const auto msg = wire::req_str(*root, "data");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return StreamError{.msg = *msg};
    });
}

std::expected<std::uint64_t, WireError> opt_number_ms(wire::ondemand::object& obj, std::string_view key) {
    const auto text = wire::opt_raw(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return wire::digits_to_u64(*text);
}

} // namespace raikiri::venues::hyperliquid
