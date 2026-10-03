#pragma once

// the v5 trade socket: one auth op, then order ops whose single arg is the rest body for the same command

#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::bybit {

inline constexpr std::string_view k_op_create = "order.create";
inline constexpr std::string_view k_op_cancel = "order.cancel";
inline constexpr std::string_view k_op_amend = "order.amend";

struct WireTradeReply {
    std::uint64_t req_id{0}; // zero when absent or not one of ours
    std::string_view op;
    std::int64_t ret_code{0};
    std::string_view ret_msg;
};

[[nodiscard]] std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame);

class TradeFrameBuilder {
public:
    explicit TradeFrameBuilder(std::uint32_t recv_window_ms = 5'000) noexcept : recv_window_ms_{recv_window_ms} {}

    [[nodiscard]] std::expected<std::string_view, BuildError>
    request(std::uint64_t id, std::string_view op, std::string_view body, UnixNanos now);

private:
    std::uint32_t recv_window_ms_;
    std::array<char, 1280> frame_{};
};

class TradeCodec final : public net::WsRpcCodec {
public:
    explicit TradeCodec(const net::Signer& signer) noexcept : signer_{&signer} {}

    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

    [[nodiscard]] std::string_view ping() const noexcept override { return protocol_.ping_frame(); }

private:
    const net::Signer* signer_;
    Protocol protocol_;
};

} // namespace raikiri::venues::bybit
