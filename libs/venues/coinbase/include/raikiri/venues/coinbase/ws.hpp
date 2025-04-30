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
inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_subscriptions_per_frame = 64;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // {"type":"subscribe","channels":[{"name":"ticker","product_ids":[..]}]}
    // Entries sharing a channel are merged into one product list, because the
    // ack comes back in that shape and a repeated channel name reads as a
    // second subscription to the operator interface.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view type, std::span<const Subscription> subscriptions);

    // Sized so a full batch of hyphenated product ids still fits:
    // BufferTooSmall costs a whole batch of subscriptions, silently.
    std::array<char, 8192> frame_{};
};

} // namespace raikiri::venues::coinbase
