#pragma once

// BingX swap-market websocket protocol: frame decompression, channel naming
// and subscription messages.
//
// THE quirk, and why this venue needs net::Inflater: BingX sends every frame
// gzip-compressed under a BINARY opcode. libs/net hands the bytes of a binary
// message to on_message unchanged — it never inspects the opcode — and
// decompresses nothing, so inflation belongs to the adapter. Past this class a
// frame is ordinary JSON, or the four bytes "Ping".
//
// A subscription is one message per stream: {"id":..,"reqType":"sub",
// "dataType":"BTC-USDT@bookTicker"}. There is no args array, so N instruments
// cost N messages — the opposite of okx, where batching protects an op-message
// budget. `id` comes back in the answer and any string works (VERIFIED: "1" is
// acknowledged), so a counter stands in for the uuid the examples show.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/inflater.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace raikiri::venues::bingx {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// Two deployments, one per market: the swap tree and the spot tree share no
// host, no path and — see the parsers — no frame shape. The config's testnet
// flag has nothing to select on either, so it is not consulted.
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// The user stream. The perpetual one is the SAME host and path as the public
// market stream with a listenKey on the query, and it takes no subscription at
// all — every account event follows the connection. Spot needs one, so a spot
// socket that connects and then says nothing is a missing subscribe rather
// than a quiet account. Both markets put the user stream on the same host as
// their public one.
[[nodiscard]] std::string_view private_stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view private_stream_path(MarketType market) noexcept;

// "/swap-market?listenKey=<key>" — the key rides the URL, so a fresh key only
// takes effect on a fresh connection.
[[nodiscard]] std::string private_stream_target(MarketType market, std::string_view key);

// The 24-hour window, on both trees. No SubKind asks for it: funding is the
// nearest thing a caller can request, and BingX answers that one over REST.
inline constexpr std::string_view k_ticker_channel = "ticker";

// The part after the '@'. Empty for a kind BingX does not stream under these
// names — including Funding, which has no channel at all (VERIFIED:
// @fundingRate, @premiumIndex and @markPriceAndFundingRate are each refused
// with 80015), and which the adapter serves by polling REST instead.
[[nodiscard]] std::string_view data_type_suffix(exec::SubKind kind, MarketType market) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
    };

    // {"id":"1","reqType":"sub","dataType":"BTC-USDT@bookTicker"}
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::string_view symbol,
                                                                     std::string_view suffix);

    // VERIFIED: "unsub" is acknowledged and the stream stops.
    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::string_view symbol,
                                                                       std::string_view suffix);

    // A whole-channel subscription: the spot user stream names a dataType with
    // no symbol in it, which the symbol+suffix form cannot express.
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_channel(std::string_view data_type);

    [[nodiscard]] static std::string_view pong_frame() noexcept { return "Pong"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view req_type, std::string_view symbol, std::string_view suffix);

    std::array<char, 256> frame_{};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::bingx
