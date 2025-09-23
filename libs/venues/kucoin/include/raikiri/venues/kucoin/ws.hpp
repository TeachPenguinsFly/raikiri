#pragma once

// KuCoin websocket protocol: topic naming, the connect URL, and subscription
// frames, public and private.
//
// A subscription is a TOPIC STRING, "/contractMarket/tickerV2:XBTUSDTM", whose
// symbol part is a comma-joined list. The venue echoes one symbol per message
// regardless (verified live), so batching keeps the topic count — what
// KuCoin's limit of 100 per connection counts — at one per kind.

#include <raikiri/domain/identifiers.hpp>
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
[[nodiscard]] std::string_view topic_for(exec::SubKind kind, MarketType market) noexcept;

// Acks, fills and cancels all arrive on ONE topic per market family — KuCoin
// has no separate execution channel — and the topic carries no symbol, so it
// is subscribed once per socket rather than per instrument. Empty for a market
// KuCoin does not serve, which the caller must refuse rather than route.
[[nodiscard]] std::string_view private_order_topic(MarketType market) noexcept;

struct Subscription {
    std::string_view topic;
    std::string_view symbol;
};

// KuCoin's documented ceiling is 100 TOPICS per connection, and batching keeps
// that at one per kind however many instruments are subscribed — so what is
// bounded here is our own bookkeeping, not the venue's limit.
inline constexpr std::size_t k_max_subscriptions_per_connection = 1024;

// What ONE connection may actually carry, counted in topic-symbol pairs. The
// number above is this adapter's own bookkeeping bound; this is the venue's,
// and exceeding it is silent — kucoin serves the first hundred and drops the
// rest without an error frame.
inline constexpr std::size_t k_max_topics_per_connection = 100;
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

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // Private topics take no symbol list and must say so: without
    // privateChannel the socket answers with the public feed of the same name
    // where one exists, and with nothing where it does not.
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    private_subscribe_frame(std::string_view topic);

    [[nodiscard]] std::string_view ping_frame() noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view type, std::span<const Subscription> subscriptions);

    std::uint64_t next_id_{1};
    std::array<char, 8192> frame_{};
    std::array<char, 64> ping_{};
};

} // namespace raikiri::venues::kucoin
