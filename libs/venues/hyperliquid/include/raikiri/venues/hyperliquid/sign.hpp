#pragma once

// every stage is pinned to the reference sdk's published vectors in the tests

#include <raikiri/net/evm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

using net::evm::Hash;
using net::evm::k_hash_bytes;
using net::evm::keccak256;
using net::evm::Signature;
using net::evm::Wallet;

// an empty vault writes 0x00, a set one 0x01 plus 20 bytes;
// expiry is omitted at zero and marked 0x00
[[nodiscard]] Hash action_hash(std::span<const std::uint8_t> action,
                               std::uint64_t nonce,
                               std::string_view vault_address,
                               std::uint64_t expires_after) noexcept;

// the domain is constant (chainId 1337); `mainnet` picks source "a" or "b"
[[nodiscard]] Hash agent_digest(const Hash& action, bool mainnet) noexcept;

// case-insensitive, since ethereum checksums addresses by case
[[nodiscard]] bool same_address(std::string_view a, std::string_view b) noexcept;

} // namespace raikiri::venues::hyperliquid
