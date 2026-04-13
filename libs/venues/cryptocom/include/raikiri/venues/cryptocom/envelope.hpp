#pragma once

#include <raikiri/venues/cryptocom/errors.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::cryptocom {

enum class EventKind : std::uint8_t {
    Heartbeat,
    Ack,
    Error,
    Book,
    BookUpdate,
    Trade,
    Ticker,
    Funding,
    EstimatedFunding,
    Mark,
    Unknown,
};

struct Event {
    EventKind kind{EventKind::Unknown};
    std::int64_t id{0};
    std::int64_t code{0};
    std::string_view instrument;
    std::string_view channel; // empty on a 40107 refusal, which names only the id
    std::string_view message;
    std::string_view data; // raw result.data

    // A subscription's first answer carries the request id; only a push has -1.
    [[nodiscard]] bool is_push() const noexcept { return id == -1; }
};

[[nodiscard]] std::expected<Event, WireError> parse_event(std::string_view frame);

struct RestStatus {
    std::int64_t code{0};
    std::string_view message;
};

[[nodiscard]] std::expected<RestStatus, WireError> parse_rest_status(std::string_view body);

} // namespace raikiri::venues::cryptocom
