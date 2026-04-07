#pragma once

// Lighter's public websocket: channel naming, subscription frames, and what
// one IP may ask of it.
//
// ONE SUBSCRIPTION PER MESSAGE. {"type":"subscribe","channel":"trade/1"} names
// a single channel and there is no batch form, so the catalogue costs its key
// count in messages — and messages, not keys, are what this venue rations.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

enum class WsBuildError : std::uint8_t {
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// THE BOOK IS THE QUOTE. There is a ticker/<id> channel, and it is not asked
// for: its top of book is the book's top, one channel fewer per instrument is
// a third of the message budget below, and the book's first frame is a full
// snapshot, so a quote arrives as soon as the ladder does.
inline constexpr std::string_view k_book_channel = "order_book";
inline constexpr std::string_view k_trade_channel = "trade";

// THE ONLY TWO `all` CHANNELS THAT MEAN IT, measured 2026-09-24. market_stats
// and spot_market_stats take `/all` and serve every market of their kind on one
// subscription. order_book/all and trade/all are refused with 30005. ticker/all
// is ACKNOWLEDGED as "ticker:all" and then serves market 0 alone — 183 of 183
// frames in eight seconds named ETH — so an `all` that is accepted is not
// evidence that it covers anything.
inline constexpr std::string_view k_perp_stats_all = "market_stats/all";
inline constexpr std::string_view k_spot_stats_all = "spot_market_stats/all";

// MEASURED 2026-09-24 against mainnet, one socket, paced under the rate below:
// 500 subscriptions acknowledged, and the 501st through 515th each answered
// {"error":{"code":23001,"message":"Too Many Subscriptions!"}} while the socket
// stayed open and kept serving the 500. The venue's page says the same.
inline constexpr std::size_t k_max_subscriptions_per_connection = 500;

// THE BINDING LIMIT, and it is PER IP, not per socket. Measured the same day:
//
//   one socket, paced 100 ms:   200 acknowledged, then 30009 "Too Many
//                               Websocket Messages!" for all 360 that followed
//   two sockets, 100 ms each:   100 acknowledged on EACH, then 30009 on both
//   one socket, paced 310 ms:   515 sent over 160 s, none refused for rate
//
// 200 a rolling minute across every socket from the address, matching the
// page. The refusal is per message and leaves the socket up, so a subscription
// sent too fast is simply not held. Protocol pings do not count: the 100 ms
// run above pinged every 20 s and still got exactly 200.
//
// So the spacing is the IP's budget spent by ONE socket, and the pool may not
// open a second: two sockets at this spacing are twice the rate the address is
// allowed, and the pool paces per connection.
inline constexpr std::uint64_t k_subscribe_spacing_ns = 310ULL * 1'000'000ULL;
inline constexpr std::size_t k_max_connections = 1;

// "order_book/1" into `out`. Returns empty for a kind this adapter does not
// read per market — funding is the one `all` stream, keyed separately.
[[nodiscard]] std::string_view
channel_name(exec::SubKind kind, std::uint32_t market_id, std::span<char> out) noexcept;

class Protocol {
public:
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    subscribe_frame(std::string_view channel);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    unsubscribe_frame(std::string_view channel);

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> frame(std::string_view type,
                                                                      std::string_view channel);

    std::array<char, 128> frame_{};
};

} // namespace raikiri::venues::lighter
