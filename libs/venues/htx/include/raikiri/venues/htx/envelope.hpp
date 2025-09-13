#pragma once

// What every HTX answer is wrapped in, on all four sockets and both REST
// trees.
//
// Market data and funding arrive from two endpoints speaking two dialects —
// `ch`/`sub` on /linear-swap-ws, `op`/`topic` on /linear-swap-notification.
// One EventKind covers both, because `ch` and `op` never appear together. The
// spot v2 socket adds a third dialect spelled `action`, and its REST answers
// carry the same `status` envelope the swap host does.
//
// The token readers below are here rather than in any one domain because two
// or more of them need each: HTX writes prices, sizes and most timestamps as
// JSON NUMBERS on the swap tree and as STRINGS on spot, so the raw token is
// the only spelling that survives both.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::htx {

// Resolved once per frame, so downstream dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Bbo,             // market.{code}.bbo
    Trade,           // market.{code}.trade.detail
    Depth,           // market.{code}.depth.step0
    Detail,          // market.{code}.detail — the 24-hour window
    Funding,         // op:notify carrying public.{code}.funding_rate
    MarketPing,      // {"ping":<ms>}
    NotifyPing,      // {"op":"ping","ts":"<ms>"}
    SpotPing,        // {"action":"ping","data":{"ts":<ms>}} — the spot v2 dialect
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
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::string_view code; // "bad-request" or "2011": see the header note
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// The swap socket answers {"op":"auth","err-code":0,...}; the spot v2 socket
// answers {"action":"req","ch":"auth","code":200,...}. Both reduce to this.
struct WireAuthReply {
    bool ok{false};
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<WireAuthReply, WireError> parse_auth_reply(std::string_view frame);

// `status` is "ok" or "error"; there is no success code to compare against.
// A body that carries neither — HTX answers an unrouted path with a Spring
// error whose `status` is the NUMBER 404 — fails to parse rather than
// reporting success, so the caller falls back to the HTTP status.
struct RestEnvelope {
    bool ok{false};
    std::string_view code; // empty when the venue named none
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// GET /api/v1/timestamp answers {"status":"ok","ts":<ms>} with no `data` at
// all, so this reads the envelope's own timestamp.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// Shared token readers ----------------------------------------------------

// A raw token minus its quotes, if it had any: one reader for `err-code`
// whether HTX wrote "bad-request" or 2011.
[[nodiscard]] std::string_view unquote(std::string_view raw) noexcept;

// HTX writes a price as a JSON NUMBER on the swap API and as a STRING on spot,
// within payloads that are otherwise the same shape. Reading the raw token and
// dropping the quotes is the one reader that survives both, and it preserves
// spellings a typed read would normalise away.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::object& obj,
                                                                std::string_view key);

// Epoch milliseconds as a digit STRING — `funding_time`, `settlement_time` and
// `settlement_date` all take that form. Never parse_decimal_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms_str(wire::ondemand::object& obj,
                                                                 std::string_view key);

// "market.BTC-USDT.bbo" and "public.BTC-USDT.funding_rate" both -> "BTC-USDT".
// A contract code carries a hyphen and never a dot, so it is what sits between
// the first and second separator whatever follows ("trade.detail" has a dot of
// its own). Not noexcept only because substr is not; callers are in guarded().
[[nodiscard]] std::string_view code_of(std::string_view topic);

} // namespace raikiri::venues::htx
