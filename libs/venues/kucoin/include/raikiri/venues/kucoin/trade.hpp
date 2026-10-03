#pragma once

// the pro order socket: a signed query opens it, the server's session frame is signed back, then spot and futures ops
// carry the rest body as args

#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/rest.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::kucoin {

inline constexpr std::string_view k_op_spot_order = "spot.order";
inline constexpr std::string_view k_op_spot_cancel = "spot.cancel";
inline constexpr std::string_view k_op_futures_order = "futures.order";
inline constexpr std::string_view k_op_futures_cancel = "futures.cancel";

struct WireTradeReply {
    std::uint64_t id{0}; // zero when absent or not one of ours
    std::string_view op;
    std::int64_t code{0}; // zero when absent
    std::string_view msg;
    bool session{false}; // the challenge the server opens with
    bool welcome{false};
};

[[nodiscard]] std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame);

class TradeFrameBuilder {
public:
    // the args are the rest body for the same order
    [[nodiscard]] std::expected<std::string_view, BuildError>
    place(std::uint64_t id, MarketType market, std::string_view body);

    [[nodiscard]] std::expected<std::string_view, BuildError>
    cancel(std::uint64_t id, const OrderCmd& cmd, const exec::InstrumentTable& table);

private:
    [[nodiscard]] std::expected<std::string_view, BuildError>
    frame(std::uint64_t id, std::string_view op, std::string_view args);

    std::array<char, 256> args_{};
    std::array<char, 1280> frame_{};
};

class TradeCodec final : public net::WsRpcCodec {
public:
    explicit TradeCodec(const Credentials& credentials) noexcept : credentials_{&credentials} {}

    // the socket's target for one connection, signed at venue time; empty without credentials
    [[nodiscard]] std::string target(UnixNanos now) const;

    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

    [[nodiscard]] bool challenged() const noexcept override { return true; }

    [[nodiscard]] std::optional<std::string> answer(std::string_view challenge) override;

private:
    const Credentials* credentials_;
};

} // namespace raikiri::venues::kucoin
