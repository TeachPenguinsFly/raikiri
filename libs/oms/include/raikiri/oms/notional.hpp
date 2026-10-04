#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/instrument.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::oms {

enum class NotionalError : std::uint8_t {
    NoPrice,     // a price-dependent contract with no price to value it at
    Unpriceable, // unknown contract kind, a coin-premium option, or no lot or contract size
};

[[nodiscard]] constexpr std::string_view to_string(NotionalError why) noexcept {
    switch (why) {
    case NotionalError::NoPrice:
        return "no price to value the order at";
    case NotionalError::Unpriceable:
        return "the instrument cannot be valued in its quote currency";
    }
    return "unknown";
}

// quote-currency value of |lots| at price, e9; linear is base units times price, inverse is the
// contracts' face in quote and ignores price, an option is its quote premium; saturates, never wraps
[[nodiscard]] std::expected<Notional128, NotionalError>
notional(const Instrument& instrument, PriceTicks price, QtyLots lots) noexcept;

// saturated to i64 for limits and messages
[[nodiscard]] FixedE9 to_e9(Notional128 value) noexcept;

} // namespace raikiri::oms
