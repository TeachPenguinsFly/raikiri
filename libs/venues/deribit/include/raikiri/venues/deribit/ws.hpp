#pragma once

// Deribit v2 public websocket protocol: channel naming and JSON-RPC frames.
//
// public/subscribe is ALL OR NOTHING. Verified live: a batch holding one
// refused channel ("ticker.BTC-PERPETUAL.raw", 13778 unauthenticated) failed
// whole and the other three were never established. A merely UNKNOWN channel
// behaves differently again — dropped silently and simply absent from the
// reply's accepted list. Hence small batches, and an accepted count compared
// against the requested count rather than assumed.

#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/endpoints.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// test.deribit.com is a separate exchange with its own book and instrument
// ids, not a header on the production host.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Longest channel built here is book.<20-char name>.none.1.100ms.
inline constexpr std::size_t k_max_channel_len = 64;

// The private feed: user.changes.{kind}.{currency}.raw. Two choices are made
// here and both are deliberate.
//
// KIND/CURRENCY rather than per-instrument, because a per-instrument
// subscription only hears about instruments something thought to name, and a
// liquidation on one nobody subscribed to is exactly the event reconciliation
// exists to catch.
//
// CHANGES rather than the separate user.orders and user.trades, because it
// delivers an order and the trades that moved it in ONE frame. Split across
// two channels, a fill's remaining quantity would have to be stitched together
// from whichever push happened to land first, and an order wrongly reported
// with nothing left is an order the OMS stops watching.
//
// `raw` is refused for an unauthenticated socket (13778, verified live), which
// is why this channel only ever goes out after public/auth succeeded.
[[nodiscard]] std::string_view user_changes_channel(std::string_view kind,
                                                    std::string_view currency,
                                                    std::span<char> into) noexcept;

// Writes the channel into `into` and returns a view over it. Empty for a kind
// Deribit does not stream under these names, or a buffer too small.
[[nodiscard]] std::string_view
channel_for(exec::SubKind kind, std::string_view instrument, std::span<char> into) noexcept;

struct Subscription {
    std::string_view channel;
};


class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
        std::int64_t id{0}; // correlates the reply that reports what was accepted
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // private/subscribe. Identical wire shape to the public one, and legal
    // only after this socket has authenticated.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    private_subscribe_frame(std::span<const Subscription> subscriptions);

    // public/auth with grant_type=client_signature: the secret never leaves
    // the process, and the digest is the same timestamp/nonce/data HMAC the
    // REST header carries with `data` left empty. Deribit remembers the token
    // for the life of this socket, so nothing after this carries one.
    [[nodiscard]] std::expected<Frame, WsBuildError> auth_frame(const net::Signer& signer,
                                                                UnixNanos now);

    // Interval is seconds and Deribit's floor is 10.
    [[nodiscard]] Frame set_heartbeat_frame(std::uint32_t interval_s);

    // The reply Deribit demands to a test_request.
    [[nodiscard]] Frame test_frame();

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    channel_frame(std::string_view scope,
                  std::string_view method,
                  std::span<const Subscription> subscriptions);

    std::array<char, ep::k_max_frame_bytes> frame_{};
    std::array<char, 24> nonce_{};
    std::array<char, 192> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::int64_t next_id_{1};
    std::uint64_t nonce_counter_{0};
};

} // namespace raikiri::venues::deribit
