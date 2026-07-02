#pragma once

// Bitget v2 websocket protocol: channel naming, subscription frames, and the
// private-stream handshake.
//
// Keepalive is the bare text "ping", answered with the bare text "pong", on
// the private socket as much as the public one. VERIFIED live on both, and
// verified in the other direction too: "ping\n" comes back as
// {"event":"error","code":30002,"msg":"Unrecognized request:ping\n"}, so it
// must be exactly those four bytes.

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    NotSigned, // credentials, or the passphrase Bitget signs alongside them
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// One host and one path for live and demo alike: Bitget switches products
// with the instType/productType parameter rather than a separate endpoint,
// so nothing here takes a testnet flag.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

// instType names a PRODUCT, not a market: the demo product answers on the
// same host with S-prefixed symbols (SBTCSUSDT), so the catalogue and the
// socket must agree on which one they asked for. Empty for a market Bitget
// does not carry, which subscribe_frame then refuses rather than sending.
[[nodiscard]] std::string_view inst_type(MarketType market, bool testnet) noexcept;

// Returns empty for a kind Bitget does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// The one private channel this adapter reads. `orders` carries fills as well
// as lifecycle — the separate `fill` channel repeats the same trade, and its
// spot element carries no clientOid, so subscribing both would double-count
// every fill and leave half of them unattributed. Positions are pulled rather
// than subscribed: GatewaySink has no channel a position push could reach.
inline constexpr std::string_view k_channel_orders = "orders";

// The account-wide instId, which is the only useful scope for order events.
inline constexpr std::string_view k_all_instruments = "default";

struct Subscription {
    std::string_view inst_type;
    std::string_view channel;
    std::string_view inst_id;
};

inline constexpr std::size_t k_max_subscriptions_per_frame = 50;

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

    // The private handshake is its own signature scheme, unrelated to REST:
    // the signed message is timestamp + "GET" + "/user/verify", and the
    // timestamp is in SECONDS. Bitget's own page says milliseconds in prose
    // while its example value ("1538054050") and its Java sample both use
    // seconds; ccxt sends seconds. The same string must appear in the frame
    // and in the digest, or the answer is 30015 with a correct key.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

    // Exposed for tests: the exact bytes the last login_frame signed.
    [[nodiscard]] std::string_view last_login_message() const noexcept;

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    std::array<char, 8192> frame_{};
    std::array<char, 64> login_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::size_t login_msg_len_{0};
};

} // namespace raikiri::venues::bitget
