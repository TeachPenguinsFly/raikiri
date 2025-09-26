#pragma once

// WEEX contract v3 public websocket: channel naming and subscription frames.
//
// A channel is "<SYMBOL>@<name>" and both halves are CASE-INSENSITIVE:
// "btcusdt@depth15" and "BTCUSDT@DEPTH15" were each accepted on a fresh socket
// and each answered with the same frames, which carry the symbol back in
// upper case. So the catalogue's own spelling goes on the wire unchanged and
// nothing here folds case.
//
// Keepalive is server-driven. WEEX sends {"event":"ping","time":"..."} roughly
// every 30 s and closes a connection that fails to answer more than ten of
// them; the answer is the constant below. There is no client-initiated ping to
// pace, which is why nothing here holds a ping timer.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// 15 and 200 are the whole tier list: @depth5 and @depth20 are refused with
// `invalid depth level`, and a bare @depth answers at 15. Subscribed at 15
// because that window is the venue's own — it holds the book at exactly 15
// levels a side and deletes what falls out — and because it feeds the quote as
// well as the ladder, so a second channel would buy nothing.
inline constexpr std::string_view k_depth_channel = "@depth15";
inline constexpr std::string_view k_trade_channel = "@trade";
inline constexpr std::string_view k_ticker_channel = "@ticker";

// What ONE connection may carry, counted in CHANNELS and not in characters.
//
// WEEX'S TWO STATED LIMITS CANNOT BOTH BE THE OPERATING ONES. Its websocket
// page says "Maximum 100 channels per connection" and "maximum 20 concurrent
// connections per IP", which multiply to 2000 — and its own catalogue needs
// 3060, three channels for each of 1020 contracts. Held to both numbers this
// gateway subscribed 534 and left 486 contracts blank, verified end to end:
// 531 of 1020 ever put a bid on the terminal's socket in 40 s.
//
// So one of them is not a limit, and it is measurable which: 600 @trade
// channels were accepted in one frame on ONE socket and all 600 pushed. 200
// is a third of that, and sixteen sockets of it covers the catalogue while
// staying under the connection cap, which is the number that would hurt to
// trip. Measured the same way afterwards: 1003 of 1020, the missing seventeen
// being contracts that did not quote at all in the minute.
//
// IF WEEX EVER STARTS ENFORCING THE HUNDRED, it will drop the excess in
// silence rather than refuse it — the shape kucoin and bitget both had here.
// What catches that is not the frame counters, which a few busy contracts keep
// high; it is counting the DISTINCT instruments that ever deliver a price.
inline constexpr std::size_t k_max_channels_per_connection = 200;

// The same page states "maximum 20 concurrent connections per IP". Sixteen
// leaves the difference for anything else this host runs against WEEX, and a
// pool that trips a CONNECTION cap is worse to recover from than one that runs
// out of channels: the venue refuses the handshake, not the subscribe.
inline constexpr std::size_t k_max_connections = 16;

// One frame carried 600 channels live, so this is a pacing choice rather than a
// venue limit: a full socket becomes four frames, which is four ticks of the
// pool's spacing rather than one burst.
inline constexpr std::size_t k_max_channels_per_frame = 25;

// The answer to a server ping. A constant, because WEEX does not echo the
// ping's own timestamp back.
inline constexpr std::string_view k_pong_frame = R"({"method":"PONG","id":1})";

// Builds "BTCUSDT@depth15" and friends into `out`. Returns empty for a kind
// WEEX does not stream.
//
// Bbo AND Depth resolve to the SAME channel: WEEX publishes no quote channel of
// its own, so the top of book comes off the ladder. A caller subscribing both
// kinds for one instrument must place one key, not two, or the pool spends two
// of a hundred on one channel.
[[nodiscard]] std::string_view
channel_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // SUBSCRIBE ONLY. WEEX answers UNSUBSCRIBE with the same verdict frame, but
    // nothing in this process can ask for one: the connection pool has no
    // per-key removal, so a channel placed on a socket stays there until the
    // socket closes. A builder with no caller is untested code that reads as a
    // capability.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> channels);

private:
    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::weex
