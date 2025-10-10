#pragma once

// Bitfinex v2 public websocket protocol: channel naming and subscription
// frames.
//
// THERE IS NO BATCH SUBSCRIBE. One `{"event":"subscribe",...}` object carries
// exactly one channel, so a frame builder that takes a span would be lying
// about what it can send — and a connection's whole ask therefore costs its
// channel count in sends, paced.
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
// stream. THE FAST ONE ARRIVES ANYWAY: the depth subscription keeps a local
// book per instrument and publishes its touch as a BBO beside this one, so
// what the ticker channel alone still carries is the 24-hour figure.
[[nodiscard]] ChannelKind kind_for(exec::SubKind kind) noexcept;

// "deriv:tBTCF0:USTF0" -> "tBTCF0:USTF0"; empty for any other key prefix. The
// acknowledgement for a status subscription carries no `symbol` field, so this
// is the only route from that frame back to an instrument.
[[nodiscard]] std::string_view symbol_from_deriv_key(std::string_view key) noexcept;

struct Subscription {
    ChannelKind kind{ChannelKind::Unknown};
    std::string_view symbol; // "tBTCUSD"
};

// WHAT ONE CONNECTION IS SERVED, MEASURED RATHER THAN READ.
//
// Bitfinex documents 25 on one page of its own reference and 30 on another.
// Neither is what it serves: on 2026-09-08 nine sockets were each asked for
// more than either, and six stopped at exactly 36 while three took 56, every
// one of them answering the rest with `10305 subscribe: limit`. Which tier a
// socket gets is the venue's choice of server — the connections were opened
// the same way, seconds apart — so the FLOOR is the only number that can be
// asked for.
//
// The refusal is loud, which is what makes 36 safe to take rather than merely
// plausible: a server stingier than the floor answers 10305, counted in
// VenueState::subscribe_refusals, instead of accepting the ask and serving
// nothing the way kucoin's 100-channel wall did.
inline constexpr std::size_t k_max_subscriptions_per_connection = 36;

class Protocol {
public:
    struct Frame {
        std::string_view text;
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(const Subscription& sub);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::int64_t chan_id);

    [[nodiscard]] static std::string_view conf_frame() noexcept;

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"event":"ping"})"; }

private:
    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::bitfinex
