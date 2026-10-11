#pragma once

// secp256k1 wallet and keccak for venues that sign with an evm key; pinned to published vectors in the tests

#include <raikiri/common/fixed_string.hpp>
#include <raikiri/config/secret.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::net::evm {

inline constexpr std::size_t k_hash_bytes = 32;

using Hash = std::array<std::uint8_t, k_hash_bytes>;

// original keccak padding (0x01); openssl's sha3-256 pads 0x06
[[nodiscard]] Hash keccak256(std::span<const std::uint8_t> data) noexcept;

[[nodiscard]] Hash keccak256(std::string_view text) noexcept;

// an optional 0x prefix; false unless the text is exactly out.size() bytes of hex
[[nodiscard]] bool from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept;

// keccak256(0x19 0x01 || domain separator || struct hash)
[[nodiscard]] Hash typed_digest(const Hash& domain_separator, const Hash& struct_hash) noexcept;

// `r` and `s` are 0x hex with leading zeroes stripped, as the reference sdks emit
struct Signature {
    FixedString<66> r{};
    FixedString<66> s{};
    std::uint8_t v{0}; // 27 or 28
};

inline constexpr std::size_t k_rsv_hex_bytes = 2 + 65 * 2;

// the 65-byte r || s || v form, 0x-prefixed lowercase hex; empty when out is short
[[nodiscard]] std::string_view rsv_hex(const Signature& sig, std::span<char> out) noexcept;

// rfc 6979 deterministic, so a digest signs to one (r, s)
class Wallet {
public:
    Wallet() = default;
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;

    // refuses a key outside [1, n-1]
    [[nodiscard]] bool load(std::string_view hex_key) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    // "0x" + 40 lowercase hex
    [[nodiscard]] std::string_view address() const noexcept;

    // low-s normalised
    [[nodiscard]] bool sign(const Hash& digest, Signature& out) const noexcept;

private:
    config::Secret key_;
    std::array<char, 42> address_{};
    bool loaded_{false};
};

} // namespace raikiri::net::evm
