#pragma once

// What every OKX answer is wrapped in, on both transports.
//
// A websocket frame names its channel in `arg` and hides the rows under
// `data`; a REST body carries `code` and hides them under the same key.
// Neither belongs to any one domain, and both have to be read first.

#include <raikiri/common/time.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::okx {

// Resolved from arg.channel once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Bbo,         // bbo-tbt — tick-by-tick top of book
    Trade,       // trades
    FundingRate, // funding-rate
    MarkPrice,   // mark-price
    Tickers,     // tickers — the only channel stating a 24-hour turnover
    Books,       // books — 400-level deltas
    Orders,      // orders — private, both lifecycle and fills
    SubscribeAck,
    LoginAck, // {"event":"login","code":"0"} — code 0 is the only success
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// A keepalive answer is the bare text "pong", not JSON, so it lands here as
// NotJson and the caller ignores it — the same path as any frame OKX adds.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: {"event":"error","code":"60018","msg":"..."}. Its
// code is a string like every other OKX code.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::int64_t k_ok = 0;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Fails rather than reporting success when `code` is missing or is not a run
// of digits: a body whose envelope cannot be read must never be mistaken for
// a body that said "0".
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// bbo-tbt and books identify the instrument only in the frame's `arg` — the
// data elements carry no instId at all, unlike trades and funding-rate.
inline std::expected<std::string_view, WireError> arg_inst_id(wire::ondemand::object& root) {
    auto arg = wire::req_object(root, "arg");
    if (!arg.has_value()) {
        return std::unexpected(arg.error());
    }
    return wire::req_str(*arg, "instId");
}

// Both transports wrap the payload in a `data` array, and every channel here
// plus every singleton REST read carries exactly one element.
template <typename Fn>
[[nodiscard]] auto with_first_data(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto array = wire::req_array(root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return std::forward<Fn>(fn)(obj);
    }
    return std::unexpected(WireError::MissingField);
}

// The rows under `data`, read into caller storage.
//
// Templates because the row reader differs per domain and nothing else does:
// the envelope, the key, the capacity refusal and the per-row walk are the
// same for orders, fills, bills, positions and acks alike.
// Both transports wrap their payload in a top-level `data` array — the
// websocket channel beside an `arg`, the REST endpoints beside `code`/`msg` —
// so one walker reads either.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_data(std::string_view body, std::span<Entry> into, Read&& read) {
    auto root = wire::session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto array = wire::req_array(*root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

// Optional fields only: OKX ships every absent value as "" rather than
// omitting it, so a missing key is a schema change, and a hard failure on one
// unread field would drop a fill we can still account for.
inline std::expected<void, WireError>
read_strings(wire::ondemand::object& obj,
             std::initializer_list<std::pair<std::string_view, std::string_view*>> fields) {
    for (const auto& [key, out] : fields) {
        const auto value = wire::opt_str(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *out = *value;
    }
    return {};
}

} // namespace raikiri::venues::okx
