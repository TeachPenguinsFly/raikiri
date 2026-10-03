#pragma once

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

enum class EventKind : std::uint8_t {
    Bbo,         // bbo-tbt — tick-by-tick top of book
    Trade,       // trades, and option-trades by family
    FundingRate, // funding-rate
    MarkPrice,   // mark-price
    Tickers,     // tickers — the only channel stating a 24-hour turnover
    Books,       // books — 400-level deltas
    Orders,      // orders — private, both lifecycle and fills
    OptSummary,  // opt-summary — greeks and vols of a whole option family
    IndexPrice,  // index-tickers
    SubscribeAck,
    LoginAck, // {"event":"login","code":"0"} — code 0 is the only success
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view inst_id; // empty on control frames
};

// the bare pong is not json and lands as NotJson
[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

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

// fails when code is missing or not digits
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// bbo-tbt and books name the instrument only in arg
inline std::expected<std::string_view, WireError> arg_inst_id(wire::ondemand::object& root) {
    auto arg = wire::req_object(root, "arg");
    if (!arg.has_value()) {
        return std::unexpected(arg.error());
    }
    return wire::req_str(*arg, "instId");
}

// every channel here and every singleton rest read carries one data element
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
std::expected<std::span<const Entry>, WireError> read_data(std::string_view body, std::span<Entry> into, Read&& read) {
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

// okx sends absent values as empty strings, so a missing key is a schema change
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
