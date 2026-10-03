#pragma once

// order ops on a logged-in private socket: the arg is the rest body plus instIdCode, the reply the rest envelope

#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::okx {

inline constexpr std::string_view k_op_order = "order";
inline constexpr std::string_view k_op_cancel = "cancel-order";
inline constexpr std::string_view k_op_amend = "amend-order";

struct WireTradeReply {
    std::uint64_t id{0}; // zero when absent or not one of ours
    std::string_view op;
    std::string_view event; // login and error frames carry this instead of an id
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame);

class TradeFrameBuilder {
public:
    // a zero code builds nothing, so the command goes over rest
    [[nodiscard]] std::expected<std::string_view, BuildError>
    request(std::uint64_t id, std::string_view op, std::string_view body, std::uint64_t inst_id_code);

private:
    std::array<char, 4096 + 128> frame_{};
};

class TradeCodec final : public net::WsRpcCodec {
public:
    TradeCodec(const net::Signer& signer, std::string_view passphrase, const std::int64_t& clock_offset_ns) noexcept
        : signer_{&signer}, passphrase_{passphrase}, clock_offset_ns_{&clock_offset_ns} {}

    TradeCodec(const net::Signer&, std::string_view, const std::int64_t&&) = delete;

    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

    [[nodiscard]] std::string_view ping() const noexcept override { return Protocol::ping_frame(); }

private:
    const net::Signer* signer_;
    std::string_view passphrase_;
    const std::int64_t* clock_offset_ns_;
    Protocol protocol_;
};

} // namespace raikiri::venues::okx
