#pragma once

// Coinbase reads the shared simdjson plumbing unchanged; the readers live in
// raikiri::wire.

#include <raikiri/venues/coinbase/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::coinbase::detail {
using namespace raikiri::wire;
} // namespace raikiri::venues::coinbase::detail
