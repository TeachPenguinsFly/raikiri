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

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> channels);

    // Only ever followed by a subscribe of the same channel, to reseed its book.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> channels);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view head, std::span<const std::string_view> channels);

    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::weex
