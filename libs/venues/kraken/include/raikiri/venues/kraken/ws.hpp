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

struct OwnedSubscription {
    std::string feed;
    std::string product_id;
};

// Keeps each feed's entries contiguous so subscribe_frame batches them: one
// frame per entry would outrun the socket's send queue.
void add_subscription(std::vector<OwnedSubscription>& set,
                      std::string_view feed,
                      std::string_view product_id);

inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_products_per_frame = 64;

// The private feeds. They ride the SAME socket shape as the public ones and
// differ only in the three credential fields the subscribe carries, which is
// why the adapter opens a second connection rather than mixing them: a public
// reconnect must not have to re-run the challenge handshake.
inline constexpr std::string_view k_feed_open_orders = "open_orders";
inline constexpr std::string_view k_feed_fills = "fills";

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // Step one of the private handshake: ask for a challenge to sign. The
    // reply is {"event":"challenge","message":"<uuid>"}, and nothing may be
    // subscribed until it arrives.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    challenge_request_frame(std::string_view api_key);

    // Step two. `signed_challenge` is the Authent chain (sign.hpp) run over
    // the challenge UUID alone — the same digest as a REST request whose
    // postData, nonce and path happen to concatenate to that UUID.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    private_subscribe_frame(std::string_view feed,
                            std::string_view api_key,
                            std::string_view original_challenge,
                            std::string_view signed_challenge);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    event_frame(std::string_view event, std::span<const Subscription> subscriptions);

    std::array<char, 4096> frame_{};
};

} // namespace raikiri::venues::kraken
