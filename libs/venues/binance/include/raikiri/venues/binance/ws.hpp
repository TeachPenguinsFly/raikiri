#pragma once

// Binance USD-M futures websocket protocol: stream naming and subscription
// frames.

#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/instrument_table.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    TooManyStreams,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;

// The multiplexed endpoint. Frames arrive wrapped; wire::unwrap peels them.
[[nodiscard]] std::string_view stream_path() noexcept;

// Binance publishes depth at fixed cadences: 100, 250 and 500 ms for futures
// diff streams.
//
// NOT THE FASTEST, BECAUSE THE FASTEST DOES NOT ARRIVE. A full perpetual
// catalogue at 100 ms is 571 diff streams times ten frames a second, and every
// socket this pool opens is read by ONE io thread — so splitting the streams
// across more sockets moves the backlog without reducing it. The venue kills
// whichever connection falls behind, abruptly enough that it reads as a
// transport failure rather than a close, and every book on that socket loses
// its delta chain at once. Measured on the deployed box: four such drops in
// seven minutes at 100 ms, against zero on bybit, gate and okx, which publish
// snapshots instead.
//
// 500 ms is a fifth of the frames for a ladder nothing here reads faster than
// that: the recorder samples depth on a five-second grid and the board prices
// once a second. A desk that needs 100 ms needs a machine that can drain it.
// THE WHOLE SUFFIX, not the cadence alone. Two constants held just the "@100ms"
// and "@1s" fragments and nothing concatenated them — suffix_for() spelled the
// full stream names out again, so editing the cadence here changed nothing at
// all and the tree looked configurable while it was not.
inline constexpr std::string_view k_depth_stream = "@depth@500ms";
inline constexpr std::string_view k_mark_price_stream = "@markPrice@1s";
inline constexpr std::string_view k_bbo_stream = "@bookTicker";
inline constexpr std::string_view k_trade_stream = "@trade";

// A connection accepts at most 1024 streams, and a single SUBSCRIBE message
// is rejected whole if it is too large, so callers batch through
// subscribe_frame() until it reports everything consumed.
//
// FILLED TO 1024 THE SOCKET GETS KICKED, and that is a rate limit rather than
// a count limit. Depth here is a diff stream at 100 ms, so a thousand streams
// is ten thousand frames a second down one TCP connection; the venue drops a
// client it decides is reading too slowly, and every book on that socket loses
// its delta chain at once. Measured on the deployed box: one socket carrying
// 1024 keys, four reopens in five minutes, and a depth-book count that
// converged to five unseeded and then jumped back to a hundred and seventy
// every time — with a reseed burst behind each jump that pushed the weight
// counter to 2297 of an allowed 2400. bybit, gate and okx on the same host
// reopened zero times; they publish snapshots rather than a diff firehose.
//
// Three hundred is the number the socket survives. It costs more connections —
// about six for a full perpetual catalogue — which binance does not meter.
inline constexpr std::size_t k_max_streams_per_connection = 300;
inline constexpr std::size_t k_max_streams_per_frame = 32;

// Builds "btcusdt@bookTicker" and friends into `out`. Returns empty for a
// kind binance does not stream. The symbol is lowercased here: binance
// rejects "BTCUSDT@bookTicker" with an error rather than silence, but only
// after the round trip.
[[nodiscard]] std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> streams);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> streams);

    [[nodiscard]] std::uint64_t last_id() const noexcept { return next_id_ - 1; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view method, std::span<const std::string_view> streams);

    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::binance
