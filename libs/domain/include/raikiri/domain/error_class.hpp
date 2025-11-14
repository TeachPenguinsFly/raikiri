#pragma once

// How a venue's refusal should be acted on.
//
// Domain vocabulary rather than exec's, for the reason PositionReport and
// FillReport are: the adapters classify it, an order event carries it across
// shm and the journal, and the screens read it back. Transport must not depend
// on the adapter layer, so the type cannot live there.

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
