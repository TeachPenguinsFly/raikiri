#pragma once

// rest codes are strings that succeed on "00000"; ws codes are json numbers in a different space

#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace raikiri::venues::bitget {

enum class EventKind : std::uint8_t {
    Top, // books1: a one-level snapshot, the quote
    Trade,
    TradeSnapshot, // the backfill a subscription opens with
    Books,         // books5/books15/books share books1's shape
    Orders,        // private: lifecycle and fills on one channel
    LoginAck,      // the private handshake's answer, distinct from a subscribe ack
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// the keepalive answer is the bare text pong and lands here as NotJson
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::string_view k_ok = "00000";

// text: "00000" and a missing code convert to the same zero
struct RestEnvelope {
    std::string_view code;
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// trade and books elements carry no instrument id, so arg is the only source
[[nodiscard]] std::expected<std::string_view, WireError> arg_inst_id(wire::ondemand::object& root);

// find_field_unordered only rewinds from the object start, so reset first
[[nodiscard]] inline std::string_view field(wire::ondemand::object& obj, std::string_view key) {
    obj.reset();
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

template <typename Fn>
[[nodiscard]] auto with_first_data(wire::ondemand::object& root,
                                   Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
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
