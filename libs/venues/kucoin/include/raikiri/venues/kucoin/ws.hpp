#pragma once

// a topic's symbol part is a comma-joined list; kucoin caps topics at 100 per connection

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

// empty for a kind kucoin does not stream under these names
[[nodiscard]] std::string_view topic_for(exec::SubKind kind, MarketType market) noexcept;

// one private topic per market family with no symbol, subscribed once per socket
[[nodiscard]] std::string_view private_order_topic(MarketType market) noexcept;

struct Subscription {
    std::string_view topic;
    std::string_view symbol;
};

inline constexpr std::size_t k_max_symbols_per_topic = 100;

// owned strings: the target carries a per-connection token
struct StreamEndpoint {
    std::string host;
    std::string port;
    std::string target;
};

// connect_id must differ per connection
[[nodiscard]] std::expected<StreamEndpoint, WsBuildError>
make_endpoint(std::string_view endpoint_url, std::string_view token, std::string_view connect_id);

// increment@10ms opens with a whole book then deltas, so there is no rest seed; 500 levels a side
inline constexpr std::string_view k_uta_book_channel = "obu";
inline constexpr std::string_view k_uta_book_depth = "increment@10ms";
inline constexpr std::string_view k_uta_trade_channel = "trade";

// public market data needs no token on this host
inline constexpr std::string_view k_uta_futures_host = "x-push-futures.kucoin.com";

// the limit is 100 client messages per 10 s per connection, pings included
inline constexpr std::uint64_t k_uta_subscribe_spacing_ns = 100ULL * 1'000'000ULL;

// the obu book carries the quote; trades and funding have no channel here
[[nodiscard]] constexpr std::string_view uta_topic_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        return k_uta_book_channel;
    case exec::SubKind::Trades:
        return k_uta_trade_channel;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

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

    // uta v2 subscribes one symbol per frame; a symbols array is refused for obu
    [[nodiscard]] std::expected<Frame, WsBuildError>
    uta_subscribe_frame(std::span<const Subscription> subscriptions);

    // private topics must set privateChannel or the socket answers with the public feed
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    private_subscribe_frame(std::string_view topic);

    [[nodiscard]] std::string_view ping_frame() noexcept;

    void set_uta_book_depth(std::string_view depth) { uta_book_depth_ = depth; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    op_frame(std::string_view type, std::span<const Subscription> subscriptions);

    std::uint64_t next_id_{1};
    std::string uta_book_depth_{k_uta_book_depth};
    std::array<char, 8192> frame_{};
    std::array<char, 64> ping_{};
};

} // namespace raikiri::venues::kucoin
