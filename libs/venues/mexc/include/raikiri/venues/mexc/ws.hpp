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

// MEXC runs no public futures testnet: contract.mexc.com is the only host, so
// unlike okx nothing here takes a flag.
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// Returns the bare channel stem, empty for a kind MEXC does not stream.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// The spot tree's channel PREFIX; the symbol goes on the end, and the depth
// one takes a level count after that.
[[nodiscard]] std::string_view spot_channel_for(exec::SubKind kind) noexcept;

// depth.full is a limit-N snapshot and the depth is part of the subscription.
// It is the only book this adapter publishes, so it carries the deepest tier
// that fits the domain snapshot. MEXC takes fixed tiers only and answers
// anything else with {"channel":"rs.error","data":"Not support limit"} —
// verified live: 5, 10, 20, 50 and 200 stream, 60, 80, 99 and 100 are refused.
inline constexpr std::uint32_t k_book_depth = 50;

// Spot's limit-depth channel offers 5, 10 and 20 — the deepest tier it has.
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

    // Futures private stream. MEXC authenticates the SOCKET, not a REST-minted
    // key: the login rides the same /edge connection the public channels use,
    // and the personal pushes follow it unbidden — there is no sub.personal.*
    // to send afterwards, so silence after login is indistinguishable from an
    // account with nothing to say. The digest covers apiKey + reqTime and
    // nothing else, which is the REST futures canonical string with its
    // parameter term empty; the two stay separate anyway, because a REST body
    // appended here would authenticate a login MEXC does not understand.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    login_frame(const net::Signer& signer, UnixNanos now);

    // Exposed for tests: the exact bytes the last login_frame signed.
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
