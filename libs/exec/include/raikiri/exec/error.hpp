#pragma once

// venue error taxonomy; adapters synthesize a terminal Rejected event for transport failures

#include <raikiri/domain/error_class.hpp>
#include <raikiri/common/fixed_string.hpp>

#include <cstdint>

namespace raikiri::exec {

// lives in domain because order events carry it across shm and the journal
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
