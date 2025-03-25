// Fuzz target for decimal parsing.
//
// Every price and quantity from every venue crosses this function, and it is
// the one place where attacker-influenced text becomes a number the system
// trades on. Beyond memory safety there is a real invariant to check: the
// parser must never return a value it cannot round-trip, because a price
// that formats back differently than it parsed is a price that means one
// thing to us and another to the venue.

#include <raikiri/common/fixed_point.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace raikiri;

    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const auto parsed = parse_decimal_e9(text);
    if (!parsed.has_value()) {
        return 0; // rejecting anything is allowed; crashing is not
    }

    // Round-trip: whatever we accepted must format back to something that
    // parses to the identical value. A mismatch means the two directions
    // disagree, which downstream shows up as an order priced off by a tick.
    char buf[32];
    const auto formatted = format_e9(*parsed, buf, sizeof buf);
    if (formatted.empty()) {
        std::abort(); // a parsed value that cannot be rendered is a defect
    }
    const auto reparsed = parse_decimal_e9(formatted);
    if (!reparsed.has_value() || reparsed->raw != parsed->raw) {
        std::abort();
    }
    return 0;
}
