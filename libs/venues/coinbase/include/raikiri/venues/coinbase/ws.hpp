#pragma once

// Coinbase Exchange public websocket protocol: channel naming and subscription
// frames.
//
// A subscription is a CHANNEL carrying its own product list:
// {"name":"ticker","product_ids":["BTC-USD"]}. The flat form the docs lead
// with ("channels":["ticker"], "product_ids":[..]) applies every product to
// every channel; the object form was verified live and is what lets one frame
// carry different products per channel.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// One public feed, no sandbox: Coinbase retired the Exchange sandbox, so
// unlike okx there is no testnet host to switch to and nothing here takes a
// flag.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind Coinbase does not stream, which the DataClient
// turns into the base class's refusal.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view product_id;
};

// Coinbase caps subscribe/unsubscribe traffic at 8 messages per second per
// IP, so subscriptions batch: one frame per instrument would spend that
// budget on eight symbols.
inline constexpr std::size_t k_max_subscriptions_per_frame = 64;

// What ONE socket in the pool is given, and it is not a number Coinbase
// publishes — it documents no per-connection subscription ceiling, and
// MEASUREMENT found none either. On 2026-09-08 a single connection to
// ws-feed.exchange.coinbase.com was asked for 1566 keys, every one of the 522
// online products on ticker, matches and level2_batch: the venue acked 522 on
// each channel and SERVED all of them — 522 distinct products delivered a
// ticker frame and 522 delivered a level2 snapshot inside 60 s, with no error
// and no close. Padded to 2610 keys with heartbeat and ticker_batch the same
// socket still acked 522 per channel and dropped nothing.
//
// So 512 is a SHARE of a socket, not its limit, and what decides the share is
// the 8 frames a second the IP is allowed: the pool paces per connection, so
// spreading the catalogue over four sockets spends that budget four ways and
// has the whole of it subscribed in a quarter of the time one socket takes.
// A reopened socket also drags only its own quarter of the books down with
// it. The number this replaced was 1024, read off nothing, and applied to the
// PROCESS — which is what refused 180 of 522 products.
inline constexpr std::size_t k_max_subscriptions_per_connection = 512;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view type, std::span<const Subscription> subscriptions);

    std::array<char, 8192> frame_{};
};

} // namespace raikiri::venues::coinbase
