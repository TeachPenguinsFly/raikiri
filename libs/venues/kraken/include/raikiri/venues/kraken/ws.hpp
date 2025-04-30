#pragma once

// Kraken Futures public websocket protocol: feed naming and subscription
// frames.
//
// There is no application-level keepalive. VERIFIED: {"event":"ping"} is
// answered with {"event":"alert","message":"Bad websocket message"}, so the
// socket is held open by websocket PING/PONG control frames, which WsClient
// already owns.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kraken {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// One host only. VERIFIED: demo-futures.kraken.com now answers 301 to a
// marketing page, so there is no demo surface left to point a testnet run at.
// The adapter refuses to start under --testnet rather than quietly opening a
// production socket, which is the failure this absence would otherwise cause.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind Kraken does not stream under these names.
[[nodiscard]] std::string_view feed_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view feed;
    std::string_view product_id;
};

inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_products_per_frame = 64;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // Consumes the leading run of subscriptions that share the FIRST entry's
    // feed and stops at the first one that does not, so a caller sweeping a
    // mixed list makes progress on every call without reordering it.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    event_frame(std::string_view event, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest feed/product pair still fits:
    // BufferTooSmall costs a whole batch of subscriptions, silently.
    std::array<char, 4096> frame_{};
};

} // namespace raikiri::venues::kraken
