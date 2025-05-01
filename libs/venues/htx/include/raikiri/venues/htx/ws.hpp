#pragma once

// HTX USDT-M swap websocket protocol: frame decompression, topic naming and
// subscription messages.
//
// THE quirk, and why this header carries an Inflater: HTX sends every frame
// gzip-compressed under a BINARY opcode, on both endpoints and in both
// directions of the keepalive. libs/net hands the bytes of a binary message
// to on_message unchanged — it never inspects the opcode — and decompresses
// nothing, so inflation belongs to the adapter. What the client sends is
// PLAIN text, verified: an uncompressed subscribe is acknowledged.
//
// Market data and funding live on two paths of one host speaking two dialects:
// /linear-swap-ws takes {"sub":"market.{code}.{topic}"}, and
// /linear-swap-notification takes {"op":"sub","topic":"public.{code}.
// funding_rate"}. VERIFIED: market.{code}.funding_rate is refused on both the
// market socket and /ws_index, so the notification path is the only funding
// push HTX has.

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace raikiri::venues::htx {

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

// HTX serves both sockets from one host; the config's testnet flag has
// nothing to select, so it is not consulted.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view market_path() noexcept;
[[nodiscard]] std::string_view notify_path() noexcept;

// The part after the contract code. Empty for a kind HTX does not stream
// under these names — including Funding, which lives on the other socket and
// so goes through funding_subscribe_frame instead.
[[nodiscard]] std::string_view topic_suffix(exec::SubKind kind) noexcept;

// Gzip -> the JSON the wire parsers read. The returned view points into this
// object's buffer and lives until the next call, which is exactly on_message's
// own contract — and makes this the buffer every string_view the parsers hand
// out points into.
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

    // {"sub":"market.BTC-USDT.bbo","id":"1"}
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::string_view code,
                                                                     std::string_view suffix);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::string_view code,
                                                                       std::string_view suffix);

    // {"op":"sub","topic":"public.BTC-USDT.funding_rate","cid":"1"}
    [[nodiscard]] std::expected<Frame, WsBuildError> funding_subscribe_frame(std::string_view code);

    [[nodiscard]] std::expected<Frame, WsBuildError> market_pong_frame(std::string_view ts);
    [[nodiscard]] std::expected<Frame, WsBuildError> notify_pong_frame(std::string_view ts);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view op, std::string_view code, std::string_view suffix);

    [[nodiscard]] std::expected<Frame, WsBuildError> pong(std::string_view head,
                                                          std::string_view ts);

    // One topic per message, so this is generous; BufferTooSmall costs a subscription silently.
    std::array<char, 256> frame_{};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::htx
