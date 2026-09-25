#pragma once

// no app-level keepalive; websocket control frames keep the socket open

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::kraken {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// one host only; the adapter refuses to start under --testnet
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

[[nodiscard]] std::string_view feed_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view feed;
    std::string_view product_id;
};

struct OwnedSubscription {
    std::string feed;
    std::string product_id;
};

// keeps each feed's entries contiguous so subscribe_frame batches them
void add_subscription(std::vector<OwnedSubscription>& set, std::string_view feed, std::string_view product_id);

// private feeds use a second connection so a public reconnect skips the challenge
inline constexpr std::string_view k_feed_open_orders = "open_orders";
inline constexpr std::string_view k_feed_fills = "fills";

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const Subscription> subscriptions);

    // step one of the private handshake; subscribe nothing until the challenge arrives
    [[nodiscard]] std::expected<Frame, WsBuildError> challenge_request_frame(std::string_view api_key);

    // step two: signed_challenge is the authent chain over the challenge uuid alone
    [[nodiscard]] std::expected<Frame, WsBuildError> private_subscribe_frame(std::string_view feed,
                                                                             std::string_view api_key,
                                                                             std::string_view original_challenge,
                                                                             std::string_view signed_challenge);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> event_frame(std::string_view event,
                                                                 std::span<const Subscription> subscriptions);

    std::array<char, 4096> frame_{};
};

} // namespace raikiri::venues::kraken
