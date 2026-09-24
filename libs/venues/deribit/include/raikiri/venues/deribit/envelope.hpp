#pragma once

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

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

// declined channels are silently absent from the subscribe reply.
// error codes are negative or positive, so the no-error sentinel is not 0
inline constexpr std::int64_t k_no_error = std::numeric_limits<std::int64_t>::min();

struct WireResponse {
    std::int64_t id{0};
    std::int64_t error_code{k_no_error};
    std::string_view error_message;
    std::size_t accepted{0};

    [[nodiscard]] bool failed() const noexcept { return error_code != k_no_error; }
};

[[nodiscard]] std::expected<WireResponse, WireError> parse_response(std::string_view frame);

// get_time answers a bare number as result
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

[[nodiscard]] std::expected<wire::ondemand::object, WireError>
push_params(wire::ondemand::object& root);

[[nodiscard]] std::expected<wire::ondemand::object, WireError>
push_data(wire::ondemand::object& root);

// raw channels push one object, timed intervals push an array
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

[[nodiscard]] std::expected<std::string_view, WireError> num_of(wire::ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(wire::ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(wire::ondemand::object& obj,
                                                                 std::string_view key);

// epoch ms would overflow parse_number_e9's 1e9 scaling; 0 means no venue time
[[nodiscard]] std::uint64_t token_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(wire::ondemand::object& obj,
                                                             std::string_view key);

} // namespace raikiri::venues::deribit
