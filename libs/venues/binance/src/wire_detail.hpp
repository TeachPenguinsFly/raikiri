#pragma once

// Wire helpers specific to binance; the shared readers live in raikiri::wire.

#include <raikiri/venues/binance/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::binance::detail {
using namespace raikiri::wire;
} // namespace raikiri::venues::binance::detail
