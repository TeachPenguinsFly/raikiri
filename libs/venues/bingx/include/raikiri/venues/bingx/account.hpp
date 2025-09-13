#pragma once

// The account's own configuration. BingX publishes no balance or ledger feed
// this adapter reads; what it does publish are two settings without which the
// user stream will not connect and an order will not route.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <expected>
#include <string_view>

namespace raikiri::venues::bingx {

// BingX's own documentation shows this answer both as a bare
// {"listenKey":".."} and wrapped in the usual envelope; both are read, because
// which one arrives decides whether the user stream connects at all.
[[nodiscard]] std::expected<std::string_view, WireError> parse_listen_key(std::string_view body);

// data.dualSidePosition, a STRING "true"/"false" rather than a JSON bool.
// True is hedge mode, where an order must name LONG or SHORT; false is one-way
// mode, where it must name BOTH and BingX refuses the others with 109400.
[[nodiscard]] std::expected<bool, WireError> parse_position_mode(std::string_view body);

} // namespace raikiri::venues::bingx
