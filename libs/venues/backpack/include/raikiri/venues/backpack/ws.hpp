#pragma once

// Stream naming and subscription frames for wss://ws.backpack.exchange.
//
// `<type>.<symbol>`, symbol in the venue's own case. Spot and perpetual
// streams share the socket; the symbol alone says which market.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::backpack {

inline constexpr std::string_view k_bbo_stream = "bookTicker.";
inline constexpr std::string_view k_trade_stream = "trade.";
inline constexpr std::string_view k_mark_price_stream = "markPrice.";
// THE 200 ms AGGREGATE, NOT THE REALTIME STREAM. Realtime depth on
// BTC_USDC_PERP was 172 frames a second of one level each (measured
// 2026-09-24), which across a catalogue is a firehose read by one io thread;
// the aggregate is at most five a second, chains by the same successor rule,
// and nothing here reads a ladder faster than once a second.
inline constexpr std::string_view k_depth_stream = "depth.200ms.";

// NOT A VENUE LIMIT — the venue did not show one. Measured 2026-09-24 on one
// socket: 2717 streams asked for in frames of 100, and every quote among them
// delivered; one frame of 2000 accepted whole. Nothing was refused, and
// nothing here could tell if it were — the venue answers a SUBSCRIBE with
// silence either way.
//
// The whole catalogue at every channel is under 500 keys, so this keeps it on
// one socket with room, and stays below anything measured.
inline constexpr std::size_t k_max_streams_per_connection = 1000;
inline constexpr std::size_t k_max_streams_per_frame = 50;

[[nodiscard]] std::string_view stream_prefix(exec::SubKind kind) noexcept;

// "bookTicker.SOL_USDC" into `out`, or empty for a kind this venue does not
// stream by symbol.
[[nodiscard]] std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

// The symbol a depth key carries, or empty when `key` is not a depth stream.
[[nodiscard]] std::string_view depth_symbol(std::string_view key) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // At most k_max_streams_per_frame of `streams`; call again with the rest.
    [[nodiscard]] std::expected<Frame, std::string_view>
    subscribe_frame(std::span<const std::string_view> streams);

private:
    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::backpack
