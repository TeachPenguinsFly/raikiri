#pragma once

// rest failures are {"message":"..."}; timestamps are iso-8601 strings with 3, 6 or 9 fractional digits

#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

enum class EventKind : std::uint8_t {
    Ticker, // ticker: best bid/ask, republished on every match
    Match,         // match, and the last_match replay sent on subscribe
    Snapshot,      // level2_batch's opening book, whole
    L2Update,      // level2_batch's changes
    Subscriptions, // the subscribe confirmation
    Error,
    Unknown, // heartbeat and status, unparsed
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view product_id; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// `reason` names the failed subscription; `message` does not
struct StreamError {
    std::string_view message;
    std::string_view reason;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

inline constexpr std::size_t k_max_ack_channels = 16;

struct AckBuffer {
    std::array<std::string_view, k_max_ack_channels> channels{};
};

// the ack names channels the venue's way (level2_batch comes back as level2_50); an empty array is the refusal
[[nodiscard]] std::expected<std::span<const std::string_view>, WireError>
parse_subscriptions(std::string_view frame, AckBuffer& storage);

// /time's `iso` field; `epoch` is a float
[[nodiscard]] std::expected<std::string_view, WireError> parse_server_time(std::string_view body);

// a coinbase rest failure body: {"message":"NotFound"}
[[nodiscard]] std::expected<std::string_view, WireError> parse_rest_error(std::string_view body);

} // namespace raikiri::venues::coinbase
