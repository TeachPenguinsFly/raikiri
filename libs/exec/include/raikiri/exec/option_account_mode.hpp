#pragma once

// the venue's account mode against the one config names, decided before options trade on that venue

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/order.hpp>

#include <raikiri/exec/execution_client.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace raikiri::exec {

struct OptionModeCheck {
    bool may_trade{false};
    std::string why{};
};

// an empty expectation compares nothing; a named one needs a reading that matches it
[[nodiscard]] inline OptionModeCheck check_option_mode(std::string_view expected,
                                                       const std::expected<std::string, VenueError>& reading) {
    if (expected.empty()) {
        return {.may_trade = true};
    }
    if (!reading.has_value()) {
        return {.why = "config wants option account mode " + std::string{expected} +
                       " and the venue did not say: " + std::string{reading.error().message.view()}};
    }
    if (*reading != expected) {
        return {.why = "config wants option account mode " + std::string{expected} + " and the venue reports " +
                       *reading};
    }
    return {.may_trade = true};
}

// a mismatch stops new risk, not the way out: this order only shrinks what the venue says is held
[[nodiscard]] constexpr bool closes_position(QtyLots net, Side side, QtyLots qty) noexcept {
    if (net.is_undef() || qty.is_undef() || qty.raw <= 0) {
        return false;
    }
    return side == Side::Buy ? net.raw < 0 && qty.raw <= -net.raw : net.raw > 0 && qty.raw <= net.raw;
}

} // namespace raikiri::exec
