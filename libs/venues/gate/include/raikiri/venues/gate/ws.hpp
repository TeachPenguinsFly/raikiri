#pragma once

// keepalive is futures.ping with a timestamp, answered on futures.pong with an empty event

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::venues::gate {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    NotSigned,         // credentials were never loaded
    MarketUnsupported, // no private channel wired for this market
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// the settle currency is in the path, so each settle currency is its own connection
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// futures private shares the public host; spot uses another host and path
[[nodiscard]] std::string_view private_host(bool testnet, MarketType market) noexcept;
[[nodiscard]] std::string_view private_path(bool testnet, MarketType market) noexcept;

// the market-data socket: options stream from their own host, the rest share the private one
[[nodiscard]] std::string_view public_host(bool testnet, MarketType market) noexcept;
[[nodiscard]] std::string_view public_path(bool testnet, MarketType market) noexcept;

// futures sockets ask for decimal sizes like rest does; spot and options send none
[[nodiscard]] std::vector<std::pair<std::string, std::string>> handshake_headers(MarketType market);

[[nodiscard]] std::string_view orders_channel(MarketType market) noexcept;
[[nodiscard]] std::string_view user_trades_channel(MarketType market) noexcept;

// gate's wildcard for every contract or pair on the account
inline constexpr std::string_view k_all_symbols = "!all";

[[nodiscard]] std::string_view channel_for(exec::SubKind kind, MarketType market) noexcept;

// gate accepts 20ms and 100ms
inline constexpr std::string_view k_depth_interval = "100ms";

// gate offers 20, 50 and 100 levels
inline constexpr std::string_view k_depth_level = "100";

// the rest seed must match: gate trims the window to exactly this depth
inline constexpr std::uint32_t k_depth_limit = 100;

// an options channel names an underlying, BTC_USDT, and gets every contract of it
struct Subscription {
    std::string_view channel;
    std::string_view contract;
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const Subscription> subscriptions,
                                                                     std::uint64_t now_s);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const Subscription> subscriptions,
                                                                       std::uint64_t now_s);

    [[nodiscard]] std::string_view ping_frame(std::uint64_t now_s, MarketType market = MarketType::Perp) noexcept;

    // each private subscribe is signed with hmac-sha512 over channel, event and time; time must match the frame's
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
