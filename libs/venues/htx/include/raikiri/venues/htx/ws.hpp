#pragma once

// htx gzips every frame under a binary opcode and libs/net does not inflate, so the adapter does

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

[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view market_path() noexcept;
[[nodiscard]] std::string_view notify_path() noexcept;

// spot's private socket is another host, and the host is part of the signature
[[nodiscard]] std::string_view spot_stream_host() noexcept;
[[nodiscard]] std::string_view spot_private_path() noexcept;

// the _cross suffix matches k_margin_mode in rest.hpp
inline constexpr std::string_view k_swap_orders_topic = "orders_cross.*";
inline constexpr std::string_view k_swap_positions_topic = "positions_cross.*";
inline constexpr std::string_view k_spot_orders_channel = "orders#*";

// mode 0 is trades only; mode 1 would repeat cancels the orders channel already sends
inline constexpr std::string_view k_spot_trades_channel = "trade.clearing#*#0";

// on the market socket, while funding is on the notification socket
inline constexpr std::string_view k_detail_topic = "detail";

// empty for kinds htx does not stream here, including Funding
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

    // {"action":"pong","data":{"ts":<ms>}}, spot v2 dialect
    [[nodiscard]] std::expected<Frame, WsBuildError> spot_pong_frame(std::string_view ts);

    // signs the rest canonical string with the notification socket's path
    [[nodiscard]] std::expected<Frame, WsBuildError> swap_auth_frame(const net::Signer& signer,
                                                                     UnixNanos now);

    // spot v2 names the parameters in lowerCamelCase and uses signatureVersion "2.1"
    [[nodiscard]] std::expected<Frame, WsBuildError> spot_auth_frame(const net::Signer& signer,
                                                                     UnixNanos now);

    // {"op":"sub","topic":"orders_cross.*","cid":"1"}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    swap_private_subscribe_frame(std::string_view topic);

    // {"action":"sub","ch":"orders#*"}
    [[nodiscard]] std::expected<Frame, WsBuildError>
    spot_private_subscribe_frame(std::string_view channel);

    // for tests: the bytes signed by the last auth frame
    [[nodiscard]] std::string_view last_auth_message() const noexcept;

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view op, std::string_view code, std::string_view suffix);

    [[nodiscard]] std::expected<Frame, WsBuildError> pong(std::string_view head,
                                                          std::string_view ts);

    // one topic per message; BufferTooSmall drops a subscription silently
    std::array<char, 256> frame_{};
    // an auth frame carries the key and a percent-encoded digest
    std::array<char, 768> auth_frame_{};
    std::array<char, 512> auth_msg_{};
    std::array<char, 256> auth_query_{};
    std::array<char, net::k_base64_bytes> signature_{};
    std::array<char, 24> timestamp_{};
    std::size_t auth_msg_len_{0};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::htx
