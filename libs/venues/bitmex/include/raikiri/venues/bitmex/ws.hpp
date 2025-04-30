#pragma once

// BitMEX realtime protocol: topic naming and subscription frames.
//
// A subscription arg is the flat text "table:symbol", so one socket carries
// every table and the endpoint never changes. Symbols are optional in the
// protocol ("quote" alone streams every contract); this adapter always names
// one, because an unscoped subscription is how a reference feed turns into a
// firehose.
//
// The keepalive is the bare text "ping" and the answer is the bare text "pong"
// — not JSON, verified live. BitMEX's banner reports heartbeatEnabled:false and
// the venue documents dropping a connection that has heard nothing, so a quiet
// contract still needs one.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitmex {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind BitMEX does not stream under these names.
[[nodiscard]] std::string_view topic_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view table;
    std::string_view symbol;
};

// BitMEX caps a connection's subscriptions and refuses the excess with a 400
// per arg rather than closing the socket, so the ceiling is enforced before the
// wire sees it.
inline constexpr std::size_t k_max_subscriptions_per_connection = 512;
inline constexpr std::size_t k_max_subscriptions_per_frame = 32;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // {"op":"subscribe","args":["quote:XBTUSD","trade:XBTUSD"]}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest table/symbol pair still fits:
    // BufferTooSmall costs a whole batch of subscriptions, silently.
    std::array<char, 4096> frame_{};
};

} // namespace raikiri::venues::bitmex
