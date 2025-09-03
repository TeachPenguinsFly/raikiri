#pragma once

// Venue error taxonomy. Classification is a first-class adapter duty: the
// generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure, and synthesizes a terminal Rejected OrderEvent for Terminal —
// venues do not push notifications for transport-level failures, so the
// adapter must convert them into lifecycle events itself (ported Go-core
// lesson).

#include <raikiri/common/fixed_string.hpp>

#include <cstdint>

namespace raikiri::exec {

enum class ErrorClass : std::uint8_t {
    Terminal,     // bad symbol/price/qty/duplicate — retrying cannot help
    Retryable,    // timeout, 5xx, transient network — bounded retry
    RateLimited,  // 429/418-style — back off per the venue's contract
    AuthFailure,  // signature/key/permission — halt venue, page a human
    Connectivity, // socket-level failure before a venue verdict existed
};

struct VenueError {
    ErrorClass cls{ErrorClass::Terminal};
    std::int64_t venue_code{0}; // venue-native numeric code when present
    FixedString<96> message{};  // truncated venue text, for logs only

    [[nodiscard]] static VenueError
    make(ErrorClass cls, std::int64_t code, std::string_view text) noexcept {
        VenueError err{.cls = cls, .venue_code = code, .message = {}};
        if (auto m = FixedString<96>::from(text.substr(0, 96))) {
            err.message = *m;
        }
        return err;
    }
};

} // namespace raikiri::exec
