#pragma once

// KuCoin Futures public websocket protocol: topic naming, the connect URL, and
// subscription frames.
//
// A subscription is a TOPIC STRING, "/contractMarket/tickerV2:XBTUSDTM", whose
// symbol part is a comma-joined list. The venue echoes one symbol per message
// regardless (verified live), so batching keeps the topic count — what
// KuCoin's limit of 100 per connection counts — at one per kind.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace raikiri::venues::kucoin {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
    BadEndpoint,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// Returns empty for a kind KuCoin does not stream under these names.
[[nodiscard]] std::string_view topic_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view topic;
    std::string_view symbol;
};

// KuCoin's documented ceiling is 100 TOPICS per connection, and batching keeps
// that at one per kind however many instruments are subscribed — so what is
// bounded here is our own bookkeeping, not the venue's limit.
inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;
inline constexpr std::size_t k_max_symbols_per_topic = 100;

// Host/target split of the bullet endpoint, with the token and connect id
// already in the query string. Owned strings: unlike every other venue here
// the target changes per connection.
struct StreamEndpoint {
    std::string host;
    std::string port;
    std::string target;
};

// `connect_id` must differ per connection; KuCoin echoes it in the welcome
// frame and uses it to tell reconnects apart.
[[nodiscard]] std::expected<StreamEndpoint, WsBuildError>
make_endpoint(std::string_view endpoint_url, std::string_view token, std::string_view connect_id);

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // Consumes the leading run of subscriptions sharing one topic, so callers
    // that group by topic get one frame per kind.
    // {"id":1,"type":"subscribe","topic":"/contractMarket/tickerV2:XBTUSDTM","response":true}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // Not static and not constant: every message needs its own id, and the
    // pong echoes it, which is the only way to tell which ping was answered.
    [[nodiscard]] std::string_view ping_frame() noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view type, std::span<const Subscription> subscriptions);

    std::uint64_t next_id_{1};
    // Sized so a full batch of the longest topic/symbol pairs still fits:
    // BufferTooSmall costs a whole batch of subscriptions, silently.
    std::array<char, 8192> frame_{};
    std::array<char, 64> ping_{};
};

} // namespace raikiri::venues::kucoin
