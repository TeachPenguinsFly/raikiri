#pragma once

// every stage is pinned to the reference sdk's published vectors in the tests

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/config/secret.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::hyperliquid {

inline constexpr std::size_t k_hash_bytes = 32;

using Hash = std::array<std::uint8_t, k_hash_bytes>;

// original keccak padding (0x01); openssl's sha3-256 pads 0x06
[[nodiscard]] Hash keccak256(std::span<const std::uint8_t> data) noexcept;

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

// `r` and `s` are 0x hex with leading zeroes stripped, as the reference sdk emits
struct Signature {
    FixedString<66> r{};
    FixedString<66> s{};
    std::uint8_t v{0}; // 27 or 28
};

// rfc 6979 deterministic, so a digest signs to one (r, s)
class Wallet {
public:
    Wallet() = default;
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;

    // refuses a key outside [1, n-1]
    [[nodiscard]] bool load(std::string_view hex_key) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    // "0x" + 40 lowercase hex; compare with the configured account before trading
    [[nodiscard]] std::string_view address() const noexcept;

    // low-s normalised, which the venue requires
    [[nodiscard]] bool sign(const Hash& digest, Signature& out) const noexcept;

private:
    config::Secret key_;
    std::array<char, 42> address_{};
    bool loaded_{false};
};

} // namespace raikiri::venues::hyperliquid
