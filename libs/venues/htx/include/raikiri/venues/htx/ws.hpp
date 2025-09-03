#pragma once

// HTX USDT-M swap websocket protocol: frame decompression, topic naming and
// subscription messages.
//
// THE quirk, and why this venue needs net::Inflater: HTX sends every frame
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
#include <raikiri/net/inflater.hpp>
#include <raikiri/net/signer.hpp>

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

// HTX serves both sockets from one host; the config's testnet flag has
// nothing to select, so it is not consulted.
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view market_path() noexcept;
[[nodiscard]] std::string_view notify_path() noexcept;

// Spot's private socket is a different HOST and a different dialect, and the
// host is a term of the handshake signature: a frame built for one endpoint
// and sent to the other authenticates as a bad key.
[[nodiscard]] std::string_view spot_stream_host() noexcept;
[[nodiscard]] std::string_view spot_private_path() noexcept;

// The private topics this adapter subscribes to. The swap's `_cross` suffix
// matches rest.hpp's k_margin_mode; an isolated-margin account would need the
// bare spellings and the isolated REST endpoints in the same change.
inline constexpr std::string_view k_swap_orders_topic = "orders_cross.*";
inline constexpr std::string_view k_swap_positions_topic = "positions_cross.*";
inline constexpr std::string_view k_spot_orders_channel = "orders#*";

// The trailing 0 asks for trade events only. Mode 1 adds cancellations, which
// the orders channel already reports — subscribing to both would deliver every
// cancel twice.
inline constexpr std::string_view k_spot_trades_channel = "trade.clearing#*#0";

// The part after the contract code. Empty for a kind HTX does not stream
// under these names — including Funding, which lives on the other socket and
// so goes through funding_subscribe_frame instead.
[[nodiscard]] std::string_view topic_suffix(exec::SubKind kind) noexcept;

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

    // {"action":"pong","data":{"ts":<ms>}} — the spot v2 dialect, which buries
    // the timestamp one level down where the other two put it at the root.
    [[nodiscard]] std::expected<Frame, WsBuildError> spot_pong_frame(std::string_view ts);

    // {"op":"auth","type":"api",...} over the swap notification socket. Signs
    // GET \n api.hbdm.com \n /linear-swap-notification \n <sorted query>, the
    // same canonical string REST uses with the socket's path in it.
    [[nodiscard]] std::expected<Frame, WsBuildError> swap_auth_frame(const net::Signer& signer,
                                                                     UnixNanos now);

    // The spot v2 handshake. Two things differ and neither is announced by the
    // failure: the parameters are named in lowerCamelCase (accessKey,
    // signatureMethod, signatureVersion, timestamp) rather than the REST
    // spelling, and signatureVersion is "2.1" where everything else says "2".
    [[nodiscard]] std::expected<Frame, WsBuildError> spot_auth_frame(const net::Signer& signer,
                                                                     UnixNanos now);

    // {"op":"sub","topic":"orders_cross.*","cid":"1"}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    swap_private_subscribe_frame(std::string_view topic);

    // {"action":"sub","ch":"orders#*"}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    spot_private_subscribe_frame(std::string_view channel);

    // Exposed for tests: the exact bytes signed by the last auth frame.
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view op, std::string_view code, std::string_view suffix);

    [[nodiscard]] std::expected<Frame, WsBuildError> pong(std::string_view head,
                                                          std::string_view ts);

    // One topic per message, so this is generous; BufferTooSmall costs a subscription silently.
    std::array<char, 256> frame_{};
    // An auth frame carries a 64-character key beside a base64 digest that is
    // percent-encoded again, so it does not fit the subscribe buffer.
    std::array<char, 768> auth_frame_{};
    std::array<char, 512> auth_msg_{};
    std::array<char, 256> auth_query_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t auth_msg_len_{0};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::htx
