#pragma once

// Hyperliquid public websocket protocol: channel naming and subscription
// frames.
//
// VERIFIED live, and why subscriptions are validated before they are sent: an
// unknown subscription TYPE earns {"channel":"error",...} and then a CLOSED
// socket, taking every other subscription with it. An unknown COIN is worse —
// nothing comes back at all, not even an ack, so a typo'd symbol is
// indistinguishable from a market that is merely quiet.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::hyperliquid {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// There is a testnet at api.hyperliquid-testnet.xyz carrying its own book.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind this venue does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view coin;
};

// MEASURED 2026-09-08 against api.hyperliquid.xyz/ws rather than read: one
// connection took exactly 1000 subscriptions and answered the 1001st with
// {"channel":"error","data":"Cannot subscribe to more than 1000 channels."} —
// and then kept serving the 1000 it had taken, so the refusal is stated but the
// coverage past it is silent. The spot catalogue's 326 pairs on four channels
// are 1304 keys, which is why one socket cannot hold it.
//
// PER CONNECTION, though the venue's own page counts it per IP: two sockets
// from this machine carried 163 pairs each on all four channels — 1304 keys —
// with 1304 acks and not one refusal.
inline constexpr std::size_t k_max_subscriptions_per_connection = 1000;

class Protocol {
public:
    // {"method":"subscribe","subscription":{"type":"bbo","coin":"BTC"}}
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    subscribe_frame(const Subscription& subscription);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    unsubscribe_frame(const Subscription& subscription);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"method":"ping"})"; }

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    method_frame(std::string_view method, const Subscription& subscription);

    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::hyperliquid
