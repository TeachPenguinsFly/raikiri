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

// UTA v2's book channel, and the mode that makes it worth having.
//
// MEASURED BACK TO BACK against classic on all 683 contracts, 2026-09-19:
// /contractMarket/level2 plus /contractMarket/tickerV2 gave 15,380 messages a
// second and 3.11 MB/s; this gave 3,230 and 0.74 — 4.76x and 4.18x, same
// symbol coverage. The reduction is the venue conflating at 10 ms, not lost
// data: counting distinct (symbol, 10 ms) pairs in the classic stream gives
// 3,452/s, agreeing within 4%.
//
// `increment@10ms` opens with a whole book and then sends deltas of it, so
// there is no REST seed to fetch and no /api/v1/level2/snapshot storm to
// meter. 500 levels a side, against the 100 this gateway publishes.
inline constexpr std::string_view k_uta_book_channel = "obu";
inline constexpr std::string_view k_uta_book_depth = "increment@10ms";
inline constexpr std::string_view k_uta_trade_channel = "trade";

// NO TOKEN, and that is what makes this a parser change rather than an account
// migration. The classic feed mints a bullet over REST and the URL carries it;
// this endpoint takes public market data from a process holding no credentials
// of any kind — verified live on 2026-09-20 from an unauthenticated client.
// KuCoin's own intro says to skip the signature and auth steps for public
// market data, and its rate-limit page counts public connections by IP.
inline constexpr std::string_view k_uta_futures_host = "x-push-futures.kucoin.com";

// A TENTH OF A SECOND, because the ceiling here is client-to-server MESSAGES
// and not topics: 100 per ten seconds per connection, counting subscribes,
// unsubscribes and pings alike. One symbol per frame means a catalogue costs
// a frame per contract, so the spacing is what keeps a large venue inside it.
inline constexpr std::uint64_t k_uta_subscribe_spacing_ns = 100ULL * 1'000'000ULL;

// What the unified feed offers, per kind. THE BOOK CARRIES THE QUOTE: `obu`
// opens with a whole ladder and the touch of an applied one is KuCoin's own
// best bid and ask, so there is no second subscription for it. Trades and
// funding has no channel here at all; it comes off the catalogue poll.
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

    // UTA v2 asks in a different shape and asks ONE SYMBOL AT A TIME. The
    // colon-joined topic is gone: the channel, the market and the symbol are
    // separate fields, and `symbols` as an array is refused for this channel
    // with "symbols are not supported for topic obu". So `consumed` is always
    // one and a catalogue costs a frame per contract.
    //
    // WHAT BOUNDS THAT is the client-to-server limit — 100 messages per ten
    // seconds per connection, counting subscribes, unsubscribes and pings
    // alike — and not any topic ceiling. It is why the send spacing for this
    // feed is a tenth of a second rather than the classic feed's.
    [[nodiscard]] std::expected<Frame, WsBuildError>
    uta_subscribe_frame(std::span<const Subscription> subscriptions);

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
