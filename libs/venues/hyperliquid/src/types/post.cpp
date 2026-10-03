#include <raikiri/venues/hyperliquid/post.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::hyperliquid {

std::expected<WirePostReply, WireError> parse_post_reply(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WirePostReply, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = wire::opt_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        WirePostReply out{};
        out.type = *channel;
        if (*channel != "post") {
            return out;
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto id = wire::opt_u64(*data, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        out.id = *id;
        auto response = wire::req_object(*data, "response");
        if (!response.has_value()) {
            return std::unexpected(response.error());
        }
        const auto type = wire::req_str(*response, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        out.type = *type;
        const auto payload = wire::opt_raw(*response, "payload");
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        out.payload = *payload;
        // an error payload is a json string of the http status text
        if (out.type == "error" && out.payload.size() >= 2 && out.payload.front() == '"') {
            out.payload = out.payload.substr(1, out.payload.size() - 2);
        }
        return out;
    });
}

std::optional<std::string_view> PostFrameBuilder::request(std::uint64_t id, std::string_view body) noexcept {
    std::array<char, 24> digits{};
    const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), id);
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    const std::array<std::string_view, 5> parts{
        R"({"method":"post","id":)",
        std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())},
        R"(,"request":{"type":"action","payload":)",
        body,
        "}}"};
    std::size_t len = 0;
    for (const auto part : parts) {
        if (len + part.size() > frame_.size()) {
            return std::nullopt;
        }
        part.copy(frame_.data() + len, part.size());
        len += part.size();
    }
    return std::string_view{frame_.data(), len};
}

std::optional<std::string> PostCodec::login(UnixNanos) {
    return std::string{};
}

net::WsRpcFrame PostCodec::classify(std::string_view payload) noexcept {
    const auto reply = parse_post_reply(payload);
    if (!reply.has_value()) {
        return {};
    }
    if (reply->type == "pong") {
        return {.kind = net::WsRpcFrame::Kind::Pong, .id = 0, .ok = true, .detail = {}};
    }
    if (reply->id == 0) {
        return {};
    }
    return {.kind = net::WsRpcFrame::Kind::Reply, .id = reply->id, .ok = reply->type == "action", .detail = {}};
}

std::string_view PostCodec::ping() const noexcept {
    return Protocol::ping_frame();
}

} // namespace raikiri::venues::hyperliquid
