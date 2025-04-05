#pragma once

// Binance USD-M futures websocket protocol: stream naming and subscription
// frames. Message building only — no sockets, no state.
//
// Two differences from bybit shape this file.
//
// Stream names are lowercase symbol + "@" + type, and everything shares one
// socket: /stream carries a multiplexed feed and each frame arrives wrapped
// in {"stream":...,"data":...}. Bybit needs a socket per market; binance
// needs one per process.
//
// There is no application-level ping. Binance sends a websocket control ping
// roughly every three minutes and disconnects a client that stops answering;
// the transport already replies to control frames, so nothing here has to.
// Adding a bybit-style app ping would send a frame binance treats as an
// unknown method and answers with an error.

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
inline constexpr std::size_t k_max_streams_per_connection = 1024;
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

    // {"method":"SUBSCRIBE","params":[...],"id":N}. The id increments per
    // frame so a response can be matched to the request that caused it.
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
