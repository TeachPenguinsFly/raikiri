#pragma once

// The Hyperliquid adapter: market data, account reads, and order entry.
//
// Order entry here signs nothing like the rest of this repo. Hyperliquid is an
// on-chain exchange: an action is msgpack-encoded, hashed with keccak-256, and
// signed with a secp256k1 key as EIP-712 typed data. There is no HMAC, no
// passphrase, and no API secret — `api_key_env` names the ACCOUNT ADDRESS,
// which is public and carries every unsigned account read, and
// `api_secret_env` names the private key of the wallet or approved API wallet
// that signs. Configure only the first and this venue reads but cannot trade;
// configure neither and it is market data alone.

#include <raikiri/exec/venue.hpp>

namespace raikiri::venues::hyperliquid {

// Explicit registration from gatewayd's main; static-init self-registration is
// banned here because the linker drops unreferenced registration objects from
// static libraries without a diagnostic.
void register_adapter();

} // namespace raikiri::venues::hyperliquid
