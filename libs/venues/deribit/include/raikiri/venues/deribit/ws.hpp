#pragma once

// Deribit v2 public websocket protocol: channel naming and JSON-RPC frames.
//
// public/subscribe is ALL OR NOTHING. Verified live: a batch holding one
// refused channel ("ticker.BTC-PERPETUAL.raw", 13778 unauthenticated) failed
// whole and the other three were never established. A merely UNKNOWN channel
// behaves differently again — dropped silently and simply absent from the
// reply's accepted list. Hence small batches, and an accepted count compared
// against the requested count rather than assumed.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// test.deribit.com is a separate exchange with its own book and instrument
// ids, not a header on the production host.
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Longest channel built here is book.<20-char name>.none.1.100ms.
inline constexpr std::size_t k_max_channel_len = 64;

// Writes the channel into `into` and returns a view over it. Empty for a kind
// Deribit does not stream under these names, or a buffer too small.
[[nodiscard]] std::string_view
channel_for(exec::SubKind kind, std::string_view instrument, std::span<char> into) noexcept;

struct Subscription {
    std::string_view channel;
};

inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;

// Small on purpose: one refusal takes its whole batch with it, so this
// amortises the round trip without risking a hundred instruments at once.
inline constexpr std::size_t k_max_subscriptions_per_frame = 16;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
        std::int64_t id{0}; // correlates the reply that reports what was accepted
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // Interval is seconds and Deribit's floor is 10.
    [[nodiscard]] Frame set_heartbeat_frame(std::uint32_t interval_s);

    // The reply Deribit demands to a test_request.
    [[nodiscard]] Frame test_frame();

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    channel_frame(std::string_view method, std::span<const Subscription> subscriptions);

    // Sized so a full batch of the longest channel fits: BufferTooSmall costs
    // a whole batch of subscriptions, silently.
    std::array<char, 2048> frame_{};
    std::int64_t next_id_{1};
};

} // namespace raikiri::venues::deribit
