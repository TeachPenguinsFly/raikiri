#pragma once

// MEXC futures public websocket protocol: channel naming and subscription
// frames.
//
// Keepalive is mandatory and inbound data does not count. VERIFIED live: a
// socket that received a push.ticker every three seconds but sent nothing was
// closed at 62 s with {"channel":"rs.error","data":"more than 60 seconds no
// response, close the channel"}. The answer to {"method":"ping"} is
// {"channel":"pong","data":<epoch ms>}.

#include <raikiri/exec/data_client.hpp>

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
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// MEXC runs no public futures testnet: contract.mexc.com is the only host, so
// unlike okx nothing here takes a flag.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns the bare channel stem, empty for a kind MEXC does not stream.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// depth.full is a limit-N snapshot and the depth is part of the subscription;
// five levels is all the BBO needs and the smallest MEXC offers.
inline constexpr std::uint32_t k_bbo_depth = 5;

struct Subscription {
    std::string_view channel;
    std::string_view symbol;
    std::uint32_t limit{0}; // depth.full only; omitted from the frame when zero
};

// Provisional: MEXC documents no per-connection subscription ceiling and 60
// ticker subscriptions on one socket were all acknowledged (verified), so this
// is a guard against unbounded growth rather than a venue-stated limit.
inline constexpr std::size_t k_max_subscriptions_per_connection = 512;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0}; // always 1: `param` names a single symbol
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"method":"ping"})"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view op,
                                                              const Subscription& subscription);

    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::mexc
