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

// The private tables are ACCOUNT-scoped: the arg is "order", not
// "order:XBTUSD". Naming a symbol there subscribes to one contract's orders and
// silently loses every other one.
inline constexpr std::string_view k_table_order = "order";
inline constexpr std::string_view k_table_execution = "execution";
inline constexpr std::string_view k_table_position = "position";

// What the private handshake signs: the verb and path of the socket itself,
// concatenated with the same absolute expiry the REST headers carry. Returns
// empty on a buffer too small to hold it.
[[nodiscard]] std::string_view auth_message(std::uint64_t expires, std::span<char> out) noexcept;

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

    // {"op":"authKeyExpires","args":["<key>",<expires>,"<signature>"]}. BitMEX
    // answers a bad signature with {"success":false,...} and then simply never
    // sends a private row, which is indistinguishable from an idle account —
    // hence the caller reports AuthFailed on the refusal rather than waiting.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    auth_frame(std::string_view api_key, std::uint64_t expires, std::string_view signature);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    private_subscribe_frame(std::span<const std::string_view> tables);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    std::array<char, 4096> frame_{};
};

} // namespace raikiri::venues::bitmex
