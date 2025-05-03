#pragma once

// Wire helpers specific to bybit; the shared readers live in raikiri::wire.

#include <raikiri/venues/bybit/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit::detail {
using namespace raikiri::wire;
} // namespace raikiri::venues::bybit::detail
