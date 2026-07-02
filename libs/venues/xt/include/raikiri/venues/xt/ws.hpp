#pragma once

// XT futures public websocket protocol: topic naming and subscription frames.
//
// The BBO comes from `depth`, a periodic top-N snapshot. XT's agg_ticker looks
// like the natural BBO channel and is not: it publishes `bp` and `ap` with no
// sizes at all, so it cannot fill a Bbo. `bbo@` and `book_ticker@` do not
// exist. depth@sym,5,100ms is the fastest self-contained top of book XT has.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// XT publishes no futures testnet endpoint, so unlike okx there is no flag:
// the demo host documented for spot does not serve fstream.
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// A topic is the channel plus whatever XT requires after the symbol. `params`
// is empty for most channels and ",5,100ms" for depth — VERIFIED: levels must
// be one of 5/10/20/50 ("levels error" otherwise) and the interval one of
// 100ms/250ms/1000ms ("interval error" otherwise).
struct Topic {
    std::string_view channel; // empty for a kind XT does not stream
    std::string_view params;
};

[[nodiscard]] Topic topic_for(exec::SubKind kind, MarketType market) noexcept;

// The rolling 24-hour window, which no SubKind names. It rides alongside the
// BBO subscription because that is the one kind asked for on every instrument
// and on both trees — XT streams no funding at all, so hanging it off
// SubKind::Funding would leave spot with no turnover. Same channel name on
// both deployments; only the payload's base column differs (see market_data.hpp).
[[nodiscard]] Topic ticker_topic() noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view symbol;
    std::string_view params;
};

// XT documents no per-frame ceiling and refused none up to five; this is a
// self-imposed bound so one malformed batch cannot cost every subscription.
inline constexpr std::size_t k_max_subscriptions_per_frame = 32;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // A TEXT frame, not a websocket ping, and the PUBLIC socket needs it as
    // much as the private one: MEASURED against stream.xt.com/public, a
    // connection answering XT's own control pings and receiving a push every
    // few hundred milliseconds was still closed with 1000 "Bye" at 30.6 s. It
    // is what the client SENDS that resets the timer.
    [[nodiscard]] static std::string_view ping_frame() noexcept;

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view method, std::span<const Subscription> subscriptions);

    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

// ---------------------------------------------------------- private ----
//
// The two private streams share nothing but the word "subscribe":
//
//   futures  wss://fstream.xt.com/ws/user, method "SUBSCRIBE" (uppercase),
//            and the listen key is GLUED TO EACH TOPIC with `@`.
//   spot     wss://stream.xt.com/private, method "subscribe" (lowercase), and
//            the token is a SEPARATE TOP-LEVEL FIELD named `listenKey`.
//
// Sending one tree's frame to the other is answered `Invalid Parameter`, which
// looks exactly like a malformed topic name.

[[nodiscard]] std::string_view private_stream_host() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;
[[nodiscard]] std::string_view spot_private_stream_host() noexcept;
[[nodiscard]] std::string_view spot_private_stream_path() noexcept;

// The subscribe answer's `code`: 0 accepted, 1 refused, 2 the key is no longer
// valid. 2 is the one that has a remedy — mint a new key and resubscribe —
// so it is named rather than folded into "refused".
inline constexpr std::int64_t k_listen_key_invalid = 2;

class PrivateProtocol {
public:
    // order and trade only. `position`, `balance` and `notify` also stream,
    // but GatewaySink carries no channel for any of them, and a subscription
    // whose pushes are dropped is a hot loop's worth of parsing for nothing —
    // positions are pulled over REST where the reconciler asks for them.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    futures_subscribe(std::string_view listen_key);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    spot_subscribe(std::string_view access_token);

    // A TEXT frame, not a websocket ping: XT closes a connection that has sent
    // no application ping for 30 s regardless of how much it received.
    [[nodiscard]] static std::string_view ping_frame() noexcept;

private:
    // The spot token is a JWT and runs to several hundred bytes.
    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::xt
