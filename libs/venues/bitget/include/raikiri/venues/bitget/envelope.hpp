#pragma once

// What every Bitget v2 answer is wrapped in, on both transports.
//
// A websocket frame names its channel in `arg` and hides the rows under
// `data`; a REST body carries `code` and hides them under the same key.
// Neither belongs to any one domain, and both have to be read first.
//
// The error code is a STRING over REST and a JSON NUMBER over the websocket,
// verified live: {"code":"40034",...} against {"event":"error","code":30001}.
// Different types AND different code spaces, so one path per transport. The
// REST envelope also succeeds on "00000", not "0" — five characters that any
// integer conversion turns into the zero a missing field would produce, which
// is why the envelope keeps the text.

#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::bitget {

// Resolved from arg.channel once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Ticker, // bid/ask, mark, index and funding on one frame
    Trade,
    Books,    // books5/books15/books share one shape
    Orders,   // private: lifecycle AND fills on one channel
    LoginAck, // the private handshake's answer, distinct from a subscribe ack
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// The keepalive answer is the bare text "pong", so it lands here as NotJson
// and is ignored — the same path as any frame Bitget adds later.
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription: {"event":"error","code":30001,"msg":"...",...}.
// Note the unquoted code; the REST envelope's is quoted.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::string_view k_ok = "00000";

// Text, not an integer: "00000" and a missing code convert to the same zero.
struct RestEnvelope {
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// trade and books elements carry no instrument id at all, so `arg` is the
// only place every channel names it.
[[nodiscard]] std::expected<std::string_view, WireError> arg_inst_id(wire::ondemand::object& root);

// Row readers reach fields out of order, and find_field_unordered only rewinds
// from the start of the object — so this resets before looking.
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

// Both transports wrap the payload in a `data` array and every channel plus
// every singleton REST read carries one element. The orderbook is the
// exception and does not use this.
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

// Walks either `data` or `data.<nested>` and reads every element into caller
// storage.
//
// Templated because the row reader differs per domain and nothing else does:
// the envelope, the nesting, the capacity refusal and the per-row walk are the
// same for orders, fills, positions and batch failures alike.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_rows(std::string_view text, std::string_view nested, std::span<Entry> storage, Read&& read) {
    auto root = wire::session().open_object(text);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    wire::ondemand::array rows;
    if (nested.empty()) {
        auto direct = wire::req_array(*root, "data");
        if (!direct.has_value()) {
            return std::unexpected(direct.error());
        }
        rows = *direct;
    } else {
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto inner = wire::req_array(*data, nested);
        if (!inner.has_value()) {
            return std::unexpected(inner.error());
        }
        rows = *inner;
    }

    std::size_t count = 0;
    for (auto item : rows) {
        if (count == storage.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::object obj;
        if (item.get_object().get(obj) != simdjson::SUCCESS) {
            return std::unexpected(WireError::WrongType);
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        storage[count] = entry;
        ++count;
    }
    return std::span<const Entry>{storage.data(), count};
}

} // namespace raikiri::venues::bitget
