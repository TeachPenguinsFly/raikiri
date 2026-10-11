#pragma once

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::htx {

enum class EventKind : std::uint8_t {
    Bbo,             // market.{code}.bbo
    Trade,           // market.{code}.trade.detail
    Depth,           // market.{code}.depth.step0
    Detail,          // market.{code}.detail, 24-hour window
    Funding,         // op:notify carrying public.{code}.funding_rate
    MarketPing,      // {"ping":<ms>}
    NotifyPing,      // {"op":"ping","ts":"<ms>"}
    SpotPing,        // {"action":"ping","data":{"ts":<ms>}}, spot v2
    AuthReply,       // the answer to a signed handshake on either private socket
    PrivateOrder,    // orders_cross.{code} / orders#{symbol}
    PrivateMatch,    // trade.clearing#{symbol}#0; the swap folds fills into orders
    PrivatePosition, // positions_cross.{code}
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract_code; // empty on control frames and keepalives

    std::string_view keepalive_ts;

    // a trade.detail ack's stamp; the last trade pushed right after it is older
    std::uint64_t trade_ack_ms{0};
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::string_view code; // "bad-request" or "2011"
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// swap answers {"op":"auth","err-code":0}, spot v2 {"action":"req","ch":"auth","code":200}
struct WireAuthReply {
    bool ok{false};
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<WireAuthReply, WireError> parse_auth_reply(std::string_view frame);

// status is "ok" or "error"; an unrouted path answers a numeric 404 status, which fails to parse
struct RestEnvelope {
    bool ok{false};
    std::string_view code; // empty when the venue named none
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

[[nodiscard]] std::string_view unquote(std::string_view raw) noexcept;

// swap writes numbers and spot strings; the raw token reads both
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::object& obj, std::string_view key);

// epoch ms as a digit string; parse_decimal_e9 would overflow int64
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_str(wire::ondemand::object& obj, std::string_view key);

// "market.BTC-USDT.bbo" -> "BTC-USDT"; codes contain hyphens, never dots
[[nodiscard]] std::string_view code_of(std::string_view topic);

} // namespace raikiri::venues::htx
