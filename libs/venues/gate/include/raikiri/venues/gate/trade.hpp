#pragma once

// the websocket api: one login, then order requests whose req_param is the rest body, answered with the rest order

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/rest.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::gate {

enum class TradeOp : std::uint8_t { Login, Place, Cancel, Amend };

// futures.order_place, spot.order_cancel and so on; empty for a market with no websocket api here
[[nodiscard]] std::string_view trade_channel(MarketType market, TradeOp op) noexcept;

struct WireTradeReply {
    std::uint64_t id{0}; // zero when absent or not one of ours
    bool ack{false};     // order_place answers once on receipt and again with the order
    std::string_view channel;
    std::uint32_t status{0};
    std::string_view result; // raw json, the rest order on success
    std::string_view label;
    std::string_view message;
};

[[nodiscard]] std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame);

class TradeFrameBuilder {
public:
    // cancel names the order and, on spot, its pair; amend adds those to the rest body
    [[nodiscard]] std::expected<std::string_view, BuildError> request(std::uint64_t id,
                                                                      MarketType market,
                                                                      TradeOp op,
                                                                      std::string_view target,
                                                                      std::string_view symbol,
                                                                      std::string_view body,
                                                                      std::uint64_t now_s);

private:
    std::array<char, 2048 + 384> frame_{};
};

class TradeCodec final : public net::WsRpcCodec {
public:
    TradeCodec(const net::Signer& signer, MarketType market, const std::int64_t& clock_offset_ns) noexcept
        : signer_{&signer}, market_{market}, clock_offset_ns_{&clock_offset_ns} {}

    TradeCodec(const net::Signer&, MarketType, const std::int64_t&&) = delete;

    // signs "api\n<channel>\n\n<time>": a login carries no req_param
    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

private:
    const net::Signer* signer_;
    MarketType market_;
    const std::int64_t* clock_offset_ns_;
    std::array<char, net::k_hex512_bytes> signature_{};
};

} // namespace raikiri::venues::gate
