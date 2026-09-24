#pragma once

// keepalive is the bare text "ping" answered by "pong", exactly those four bytes

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

// one host for live and demo; the product parameter switches them
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

// demo answers on the same host with S-prefixed symbols; empty for an unsupported market
[[nodiscard]] std::string_view inst_type(MarketType market, bool testnet) noexcept;

[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// orders carries fills too, so fill is not subscribed; positions are polled
inline constexpr std::string_view k_channel_orders = "orders";

inline constexpr std::string_view k_all_instruments = "default";

struct Subscription {
    std::string_view inst_type;
    std::string_view channel;
    std::string_view inst_id;
};

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

    // signs timestamp + "GET" + "/user/verify" with the timestamp in seconds, same string in frame and digest
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

    // for tests: the bytes the last login_frame signed
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
