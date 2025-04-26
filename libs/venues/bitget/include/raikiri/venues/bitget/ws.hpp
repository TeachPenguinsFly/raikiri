#pragma once

// Bitget v2 public websocket protocol: channel naming and subscription frames.
// Message building only — no sockets, no state.
//
// A subscription is an OBJECT carrying THREE fields, not two:
// {"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"}. The
// instType is not optional and not inferable from the symbol — BTCUSDT names
// a contract under USDT-FUTURES and another under SUSDT-FUTURES (the demo
// product), so omitting it is refused rather than defaulted.
//
// Keepalive is the bare text "ping", answered with the bare text "pong".
// VERIFIED live, and verified in the other direction too: "ping\n" comes back
// as {"event":"error","code":30002,"msg":"Unrecognized request:ping\n"}, so
// it must be exactly those four bytes.
//
// Bitget documents a 30 s idle timeout and a 10 message/second cap per
// connection, which is why subscriptions batch — one frame per instrument
// would spend the whole budget on a few hundred symbols.

#include <raikiri/exec/data_client.hpp>

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
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// One host and one path for live and demo alike: Bitget switches products
// with the instType/productType parameter rather than a separate endpoint,
// so nothing here takes a testnet flag.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// The demo product answers on the same host with S-prefixed symbols
// (SBTCSUSDT), so the catalogue and the socket must agree on which one they
// asked for.
[[nodiscard]] std::string_view inst_type(bool testnet) noexcept;

// Returns empty for a kind Bitget does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view inst_type;
    std::string_view channel;
    std::string_view inst_id;
};

inline constexpr std::size_t k_max_subscriptions_per_connection = 1000;
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

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view op, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest instType/channel/instId triple
    // still fits: BufferTooSmall costs a whole batch of subscriptions,
    // silently.
    std::array<char, 8192> frame_{};
};

} // namespace raikiri::venues::bitget
