#pragma once

// Bitfinex v2 public websocket protocol: channel naming and subscription
// frames.
//
// THERE IS NO BATCH SUBSCRIBE. One `{"event":"subscribe",...}` object carries
// exactly one channel, so a frame builder that takes a span would be lying
// about what it can send. 40 ticker subscriptions on one socket were accepted
// without a refusal (verified live), so the ceiling below is our own
// conservatism rather than an observed wall.
//
// UNSUBSCRIBE IS BY CHANNEL ID, not by name: `{"event":"unsubscribe",
// "chanId":17470}`. The venue never accepts the symbol back, which is the
// clearest statement that the channel id — not the symbol — is the identity of
// a stream here.
//
// Keepalive: Bitfinex pushes `[CHAN_ID,"hb"]` on every subscribed channel
// about every 15 s, so silence is already diagnostic. The app-level
// `{"event":"ping"}` is kept because a socket with zero live subscriptions
// gets no heartbeats at all; it answers `{"event":"pong","cid":...,"ts":...}`
// (verified).

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::bitfinex {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// What a channel id, once bound, turns out to carry. Resolved from the
// subscribe acknowledgement so that dispatch on a data frame is an integer
// compare against the registry rather than a string compare per message.
enum class ChannelKind : std::uint8_t {
    Ticker,
    Trades,
    Book,
    DerivStatus,
    Unknown,
};

// The `status` channel serves several unrelated feeds under one name
// (`deriv:<symbol>`, `liq:global`), so the key decides, not the channel.
[[nodiscard]] ChannelKind channel_kind(std::string_view channel, std::string_view key) noexcept;

[[nodiscard]] std::string_view channel_name(ChannelKind kind) noexcept;

// Returns Unknown for a kind Bitfinex does not stream publicly.
//
// KNOWN LIMIT, measured rather than assumed. `ticker` is NOT tick-by-tick the
// way okx's bbo-tbt or binance's bookTicker are: over one minute on tBTCUSD it
// pushed 5 frames while `book` at P0/F0 pushed 1261, and both are the same top
// of book. So a BBO sourced here is roughly a 12-second sample, not a quote
// stream. `book` is the fix and this adapter already parses it; what it also
// needs is a local book kept per instrument, which is state no other adapter
// here carries and is out of scope for market data. Recorded so the next
// person measures the alternative instead of rediscovering the number.
[[nodiscard]] ChannelKind kind_for(exec::SubKind kind) noexcept;

// "deriv:tBTCF0:USTF0" -> "tBTCF0:USTF0"; empty for any other key prefix. The
// acknowledgement for a status subscription carries no `symbol` field, so this
// is the only route from that frame back to an instrument.
[[nodiscard]] std::string_view symbol_from_deriv_key(std::string_view key) noexcept;

struct Subscription {
    ChannelKind kind{ChannelKind::Unknown};
    std::string_view symbol; // "tBTCUSD"
};

// Bitfinex documents a per-connection channel limit; 40 were accepted live, so
// this is a safety valve rather than the venue's number.
inline constexpr std::size_t k_max_subscriptions_per_connection = 30;

class Protocol {
public:
    struct Frame {
        std::string_view text;
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(const Subscription& sub);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::int64_t chan_id);

    // {"event":"conf","flags":32768} — asks Bitfinex to append its send
    // timestamp to every data frame. Worth the request because the `ticker`
    // channel's payload carries NO time of its own, so without it a BBO's
    // ts_event can only be ingest time and every latency measurement over this
    // venue collapses to zero. The flag appends at the TAIL of the envelope,
    // which is the only place a positional format can grow without renaming
    // anything.
    [[nodiscard]] static std::string_view conf_frame() noexcept;

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"event":"ping"})"; }

private:
    // Sized so the longest channel/symbol pair still fits: BufferTooSmall
    // costs a whole subscription, silently.
    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::bitfinex
