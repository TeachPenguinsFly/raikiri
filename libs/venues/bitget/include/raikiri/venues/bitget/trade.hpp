#pragma once

// the v2 private socket's trade op: login, then place-order and cancel-order with the rest fields as params

#include <raikiri/config/secret.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::bitget {

inline constexpr std::string_view k_channel_place_order = "place-order";
inline constexpr std::string_view k_channel_cancel_order = "cancel-order";

// the socket answers a trade with code 0 where rest answers 00000
inline constexpr std::string_view k_ws_ok = "0";

struct WireTradeReply {
    std::uint64_t id{0}; // zero when absent or not one of ours
    std::string_view event;
    std::string_view code; // unquoted; the docs show it as a number and as a string
    std::string_view msg;
};

[[nodiscard]] std::expected<WireTradeReply, WireError> parse_trade_reply(std::string_view frame);

class TradeFrameBuilder {
public:
    [[nodiscard]] std::expected<std::string_view, BuildError> place(std::uint64_t id,
                                                                    const OrderCmd& cmd,
                                                                    const exec::InstrumentTable& table,
                                                                    std::string_view product,
                                                                    std::string_view inst_type);

    [[nodiscard]] std::expected<std::string_view, BuildError>
    cancel(std::uint64_t id, const OrderCmd& cmd, const exec::InstrumentTable& table, std::string_view inst_type);

private:
    [[nodiscard]] std::expected<std::string_view, BuildError> frame(std::uint64_t id,
                                                                    std::string_view inst_type,
                                                                    std::string_view inst_id,
                                                                    std::string_view channel,
                                                                    std::string_view params);

    std::array<char, 512> params_{};
    std::array<char, 1024> frame_{};
};

class TradeCodec final : public net::WsRpcCodec {
public:
    // the offset is the venue clock's lead over ours; bitget refuses a login 30 s off
    TradeCodec(const net::Signer& signer,
               const config::Secret& passphrase,
               const std::int64_t& clock_offset_ns) noexcept
        : signer_{&signer}, passphrase_{&passphrase}, clock_offset_ns_{&clock_offset_ns} {}

    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

    [[nodiscard]] std::string_view ping() const noexcept override { return Protocol::ping_frame(); }

private:
    const net::Signer* signer_;
    const config::Secret* passphrase_;
    const std::int64_t* clock_offset_ns_;
    Protocol protocol_;
};

} // namespace raikiri::venues::bitget
