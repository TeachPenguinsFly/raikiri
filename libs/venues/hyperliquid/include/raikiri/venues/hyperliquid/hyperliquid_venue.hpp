#pragma once

// api_key_env names the account address and api_secret_env the signing wallet's private key

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::hyperliquid {

// registered explicitly; the linker drops unreferenced static registration objects
void register_adapter();

} // namespace raikiri::venues::hyperliquid
