#pragma once

// Gate.io v4 futures public websocket protocol: channel naming and
// subscription frames.
//
// Keepalive is a JSON frame on its own channel — {"time":t,"channel":
// "futures.ping"} — answered on "futures.pong" with an EMPTY event string
// (verified live). It carries a timestamp, so unlike okx's four-byte "ping"
// it cannot be a constant.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    NotSigned,        // credentials were never loaded
    MarketUnsupported, // no private channel wired for this market
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// The settle currency is baked into the path, so a USDT gateway and a
// BTC-settled one are different connections rather than different frames.
//
// The testnet host is Gate's documented one and it answered 502 on every
// probe in 2026-08 — REST testnet (a different host again) works, this does
// not. Kept rather than removed because the flag has to point somewhere and
// an outage is not a reason to hardcode the live host behind --testnet.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Private streams. Futures shares the public host and settle-scoped path;
// spot is a different host AND a different path, so the two private sockets
// have nothing in common but the frame grammar.
[[nodiscard]] std::string_view private_host(bool testnet, MarketType market) noexcept;
[[nodiscard]] std::string_view private_path(bool testnet, MarketType market) noexcept;

// Empty for a market with no private channel wired here.
[[nodiscard]] std::string_view orders_channel(MarketType market) noexcept;
[[nodiscard]] std::string_view user_trades_channel(MarketType market) noexcept;

// Every contract / currency pair on the account. Gate's own wildcard, not a
// convention of ours.
inline constexpr std::string_view k_all_symbols = "!all";

// Returns empty for a kind Gate does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind, MarketType market) noexcept;

// futures.order_book_update needs both in its payload. Gate accepts 20ms and
// 100ms (both verified live); 100 ms is chosen because a finer cadence would
// buy bandwidth and no signal.
inline constexpr std::string_view k_depth_interval = "100ms";

// 20, 50 and 100 are the whole tier list: 200 and 400 are refused with
// `provided level not supported`, so 100 is both the deepest tier and the
// most k_book_levels can carry.
inline constexpr std::string_view k_depth_level = "100";

// The same number the REST seed must ask for. Gate maintains exactly this many
// levels and deletes what falls out of the window, so seeding at a different
// depth gives a book whose edge the stream never corrects.
inline constexpr std::uint32_t k_depth_limit = 100;

struct Subscription {
    std::string_view channel;
    std::string_view contract;
};

inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_subscriptions_per_frame = 64;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions, std::uint64_t now_s);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions, std::uint64_t now_s);

    [[nodiscard]] std::string_view ping_frame(std::uint64_t now_s) noexcept;

    // A private subscribe carries its own signature: HMAC-SHA512 over
    // "channel=<c>&event=<e>&time=<t>", hex, in an `auth` object beside the
    // payload. There is no connection-wide login for the push channels — every
    // subscribe re-authenticates, and the `time` inside the digest must be the
    // same one the frame carries or Gate answers code 4.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    private_subscribe_frame(std::string_view channel,
                            std::span<const std::string_view> payload,
                            const net::Signer& signer,
                            std::uint64_t now_s);

    // Exposed for tests: the exact bytes signed by the last private frame.
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions, std::uint64_t now_s);

    std::array<char, 8192> frame_{};
    std::array<char, 64> ping_{};
    std::array<char, 128> auth_msg_{};
    std::array<char, net::k_hex512_bytes> signature_{};
    std::size_t auth_msg_len_{0};
};

} // namespace raikiri::venues::gate
