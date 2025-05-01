#pragma once

// BingX swap-market websocket protocol: frame decompression, channel naming
// and subscription messages.
//
// THE quirk, and why this header carries an Inflater: BingX sends every frame
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

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace raikiri::venues::bingx {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

enum class InflateError : std::uint8_t {
    NotGzip,   // no 1f 8b magic, or a compression method that is not deflate
    Truncated, // the gzip header runs past the end of the frame
    TooLarge,  // expands past the ceiling: a decompression bomb, not a quote
    Corrupt,   // the deflate stream itself does not decode
};

[[nodiscard]] std::string_view to_string(InflateError err) noexcept;

// BingX serves market data from one host and one path; the config's testnet
// flag has nothing to select, so it is not consulted.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// The part after the '@'. Empty for a kind BingX does not stream under these
// names — including Funding, which has no channel at all (VERIFIED:
// @fundingRate, @premiumIndex and @markPriceAndFundingRate are each refused
// with 80015), and which the adapter serves by polling REST instead.
[[nodiscard]] std::string_view data_type_suffix(exec::SubKind kind) noexcept;

// Gzip -> the JSON the wire parsers read. The returned view points into this
// object's buffer and lives only until the next call — on_message's own
// contract exactly.
class Inflater {
public:
    Inflater();
    ~Inflater();
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;

    [[nodiscard]] std::expected<std::string_view, InflateError> inflate(std::string_view frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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

    [[nodiscard]] static std::string_view pong_frame() noexcept { return "Pong"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view req_type, std::string_view symbol, std::string_view suffix);

    std::array<char, 256> frame_{};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::bingx
