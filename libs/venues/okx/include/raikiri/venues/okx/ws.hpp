#pragma once

// keepalive is the bare text ping, exactly four bytes; okx drops sockets idle for 30 s

#include <raikiri/common/time.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    NotSigned,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// demo is a separate host with its own book; rest demo is a header instead
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;

[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// public channels name an instId; orders names an instType to cover every instrument
struct Subscription {
    std::string_view channel{};
    // braced so a designated initialiser may omit it without a gcc warning
    std::string_view inst_id{};
    std::string_view inst_type{};
};

inline constexpr std::string_view k_orders_channel = "orders";

// the only public channel with 24h turnover; it rides along with the funding subscription
inline constexpr std::string_view k_tickers_channel = "tickers";

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // {"op":"subscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"}]}
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const Subscription> subscriptions);

    // the timestamp is epoch seconds and the message is timestamp + GET + /users/self/verify;
    // okx expires it 30 s later
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, std::string_view passphrase, UnixNanos now);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

    [[nodiscard]] std::string_view last_login_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view op,
                                                              std::span<const Subscription> subscriptions);

    std::array<char, 8192> frame_{};
    std::array<char, 64> login_msg_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::size_t login_msg_len_{0};
};

} // namespace raikiri::venues::okx
