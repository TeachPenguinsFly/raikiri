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

// Binance publishes depth at fixed cadences; 100 ms is the fastest offered
// for futures diff streams.
inline constexpr std::string_view k_depth_cadence = "@100ms";
inline constexpr std::string_view k_mark_price_cadence = "@1s";

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
