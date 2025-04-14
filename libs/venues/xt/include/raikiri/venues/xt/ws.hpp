#pragma once

// XT futures public websocket protocol: topic naming and subscription frames.
// Message building only — no sockets, no state.
//
// Four differences from okx shape this file.
//
// A subscription is a single STRING, not an object: "depth@btc_usdt,5,100ms".
// The channel, the symbol and any parameters are joined by '@' and commas, so
// a topic is assembled rather than described.
//
// There is NO application-level keepalive. The text "ping" is answered with
// the bare text "Invalid parameter" and {"method":"ping"} with "No enum
// constant ... Method.PING", so the transport's own websocket ping/pong frames
// are the only keepalive — WsConfig already runs them.
//
// The BBO comes from `depth`, a periodic top-N snapshot. XT's agg_ticker looks
// like the natural BBO channel and is not: it publishes `bp` and `ap` with no
// sizes at all, so it cannot fill a Bbo. `bbo@` and `book_ticker@` do not
// exist. depth@sym,5,100ms is the fastest self-contained top of book XT has.
//
// There is NO funding channel. funding_rate@, funding@ and fundingRate@ are
// each refused with "No enum constant ... Topic.x", so funding is polled over
// REST instead — see rest.hpp.
//
// Every claim above was checked against wss://fstream.xt.com/ws/market in
// 2026-08 by subscribing and reading what came back. Frames are uncompressed
// UTF-8 JSON; nothing here needs binary frame support.

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
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// A topic is the channel plus whatever XT requires after the symbol. `params`
// is empty for most channels and ",5,100ms" for depth — VERIFIED: levels must
// be one of 5/10/20/50 ("levels error" otherwise) and the interval one of
// 100ms/250ms/1000ms ("interval error" otherwise).
struct Topic {
    std::string_view channel; // empty for a kind XT does not stream
    std::string_view params;
};

[[nodiscard]] Topic topic_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view symbol;
    std::string_view params;
};

inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
// XT documents no per-frame ceiling and refused none up to five; this is a
// self-imposed bound so one malformed batch cannot cost every subscription.
inline constexpr std::size_t k_max_subscriptions_per_frame = 32;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // {"method":"subscribe","params":["depth@btc_usdt,5,100ms"],"id":"1"}
    //
    // A batch containing one bad topic still subscribes the good ones and
    // answers with a single error naming only the first failure, so an id per
    // FRAME is as much correlation as XT offers.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view method, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest topic still fits: BufferTooSmall
    // costs a whole batch of subscriptions, silently.
    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::xt
