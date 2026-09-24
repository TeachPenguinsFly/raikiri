#pragma once

// inbound data does not count as keepalive; the venue closes a silent socket after 60 s

#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::mexc {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    NotSigned,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// no public futures testnet
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// bare channel stem, empty for a kind mexc does not stream
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// channel prefix; the symbol and, for depth, a level count follow
[[nodiscard]] std::string_view spot_channel_for(exec::SubKind kind) noexcept;

// deepest supported tier that fits the snapshot; tiers are 5, 10, 20, 50 and 200
inline constexpr std::uint32_t k_book_depth = 50;

// 5, 10 or 20
inline constexpr std::uint32_t k_spot_book_depth = 20;

struct Subscription {
    std::string_view channel;
    std::string_view symbol;
    std::uint32_t limit{0}; // depth.full only; omitted from the frame when zero
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0}; // always 1: `param` names a single symbol
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    spot_subscribe_frame(const Subscription& subscription);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // the login rides /edge and personal pushes follow unbidden;
    // the digest covers apiKey + reqTime only
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, UnixNanos now);

    // for tests: the bytes the last login_frame signed
    [[nodiscard]] std::string_view last_login_message() const noexcept;

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"method":"ping"})"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    spot_op_frame(std::string_view op, const Subscription& subscription);

    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view op,
                                                              const Subscription& subscription);

    std::array<char, 256> frame_{};
    std::array<char, 256> login_frame_{};
    std::array<char, 160> login_msg_{};
    std::array<char, net::k_hex_bytes> login_sig_{};
    std::size_t login_msg_len_{0};
};

} // namespace raikiri::venues::mexc
