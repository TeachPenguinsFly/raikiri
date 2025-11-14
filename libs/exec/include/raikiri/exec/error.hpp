#pragma once

// Venue error taxonomy. Classification is a first-class adapter duty: the
// generic gateway retries Retryable, backs off RateLimited, halts on
// AuthFailure, and synthesizes a terminal Rejected OrderEvent for Terminal —
// venues do not push notifications for transport-level failures, so the
// adapter must convert them into lifecycle events itself (ported Go-core
// lesson).

#include <raikiri/domain/error_class.hpp>
#include <raikiri/common/fixed_string.hpp>

#include <cstdint>

namespace raikiri::exec {

// Lives in domain, because an order event carries it across shm and the
// journal. Spelled exec::ErrorClass everywhere it is used.
using ErrorClass = raikiri::ErrorClass;

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
