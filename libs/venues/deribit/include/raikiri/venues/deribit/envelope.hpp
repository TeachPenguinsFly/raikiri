#pragma once

// What every Deribit answer is wrapped in.
//
// Deribit speaks JSON-RPC v2 on BOTH sockets and over REST, so one envelope
// covers all three: a push is a `method` of "subscription" with the payload
// under params.data, and everything else is a reply carrying `result` or
// `error`. src/frames.cpp is the one dispatcher both clients share.
//
// The token readers below are here rather than in any one domain because two
// or more of them need each: every number arrives in whatever form Deribit
// chose ("0.5", "2.7e4"), and decoding is parse_number_e9's job at the mapping
// edge, never this layer's.

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// Resolved from the channel prefix once, so dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Bbo,        // book.{instrument}.none.1.100ms — depth-1 grouped snapshot
    Trade,      // trades.{instrument}.100ms
    Ticker,     // ticker.{instrument}.100ms — mark, index and funding in one
    Book,       // book.{instrument}.100ms — incremental, change_id chained
    UserChange, // user.changes.{kind}.{currency}.raw — private, authenticated
    Heartbeat,  // server liveness probe; a test_request needs an answer
    Response,   // reply to something we sent: result or error
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view instrument;  // second channel segment; empty on replies
    bool needs_test_reply{false}; // Heartbeat only: type == "test_request"
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A subscribe reply is {"id":N,"result":[<accepted channels>]}. Declined
// channels are SILENTLY ABSENT from that array (verified: an unknown
// instrument yields "result":[] with no error), so the accepted count is the
// only evidence a subscription took. Non-array results count zero.
// Deribit uses both negative (JSON-RPC standard) and positive codes, so the
// "no error" sentinel cannot be 0 the way an OKX envelope's is.
inline constexpr std::int64_t k_no_error = std::numeric_limits<std::int64_t>::min();

struct WireResponse {
    std::int64_t id{0};
    std::int64_t error_code{k_no_error};
    std::string_view error_message;
    std::size_t accepted{0};

    [[nodiscard]] bool failed() const noexcept { return error_code != k_no_error; }
};

// REST answers with the identical envelope, so this one reader serves both
// transports; absence of `error` is success.
[[nodiscard]] std::expected<WireResponse, WireError> parse_response(std::string_view frame);

// /public/get_time answers a bare number as `result`, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

// Push payload -------------------------------------------------------------

// A subscription push carries `params`; every implemented one nests its
// payload one level further down, in params.data.
[[nodiscard]] std::expected<wire::ondemand::object, WireError>
push_params(wire::ondemand::object& root);

[[nodiscard]] std::expected<wire::ondemand::object, WireError>
push_data(wire::ondemand::object& root);

// Reads `key` whether Deribit spelled it as one object or as an array of
// them. The raw channels push a single row and the timed intervals push a
// batch; refusing either shape would silently drop half the private stream.
template <typename Row, typename Reader>
[[nodiscard]] std::expected<std::size_t, WireError> read_rows(wire::ondemand::object& parent,
                                                              std::string_view key,
                                                              std::span<Row> into,
                                                              Reader reader) {
    wire::ondemand::value value;
    if (const auto ec = parent.find_field_unordered(key).get(value); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    std::size_t count = 0;

    wire::ondemand::array array;
    if (value.get_array().get(array) == simdjson::SUCCESS) {
        for (auto entry : array) {
            if (count == into.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto row = reader(obj);
            if (!row.has_value()) {
                return std::unexpected(row.error());
            }
            into[count] = *row;
            ++count;
        }
        return count;
    }

    wire::ondemand::object obj;
    if (value.get_object().get(obj) != simdjson::SUCCESS) {
        return std::unexpected(WireError::WrongType);
    }
    if (into.empty()) {
        return std::unexpected(WireError::TooManyEntries);
    }
    const auto row = reader(obj);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    into[0] = *row;
    return std::size_t{1};
}

// Shared token readers ----------------------------------------------------

// Reads a JSON number as its raw token, over the caller's buffer, in whatever
// form Deribit chose ("0.5", "2.7e4"); decoding is parse_number_e9's job at
// the mapping edge.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// Epoch milliseconds. These must NOT go through parse_number_e9: it scales by
// 1e9 and an epoch-ms value overflows int64 on the way. std::from_chars over
// the raw token is the whole conversion; anything else yields 0, which the
// mapper reads as "venue gave us no time" and replaces with ingest time.
[[nodiscard]] std::uint64_t token_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::deribit
