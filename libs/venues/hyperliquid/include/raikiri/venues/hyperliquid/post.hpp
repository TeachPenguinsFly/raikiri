#pragma once

// websocket post: the signed /exchange payload under a request id; the action's signature is the only auth

#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::hyperliquid {

// the venue allows 100 posts in flight across every socket of one ip
inline constexpr std::size_t k_post_in_flight = 90;

// the rate budget meters posts under this name; config/venues.yaml holds the per-ip message limit
inline constexpr std::string_view k_post_budget_target = "ws:post";

struct WirePostReply {
    std::uint64_t id{0};
    std::string_view type;    // "action" | "error"
    std::string_view payload; // the /exchange reply for an action, the http status text for an error
};

[[nodiscard]] std::expected<WirePostReply, WireError> parse_post_reply(std::string_view frame);

class PostFrameBuilder {
public:
    // `body` is the /exchange request body for the same command
    [[nodiscard]] std::optional<std::string_view> request(std::uint64_t id, std::string_view body) noexcept;

private:
    std::array<char, 2048> frame_{};
};

class PostCodec final : public net::WsRpcCodec {
public:
    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;
    [[nodiscard]] std::string_view ping() const noexcept override;
};

} // namespace raikiri::venues::hyperliquid
