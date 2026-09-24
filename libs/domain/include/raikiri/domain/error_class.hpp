#pragma once

#include <cstdint>

namespace raikiri {

enum class ErrorClass : std::uint8_t {
    Terminal,     // bad symbol/price/qty/duplicate — retrying cannot help
    Retryable,    // timeout, 5xx, transient network — bounded retry
    RateLimited,  // 429/418-style — back off per the venue's contract
    AuthFailure,  // signature/key/permission — halt venue, page a human
    Connectivity, // socket-level failure before a venue verdict existed
};

} // namespace raikiri
