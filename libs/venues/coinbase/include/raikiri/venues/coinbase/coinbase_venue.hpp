#pragma once

// coinbase exchange (api.exchange.coinbase.com): spot only; public reads need no credentials

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::coinbase {

// explicit registration: static libraries drop unreferenced self-registration objects
void register_adapter();

} // namespace raikiri::venues::coinbase
