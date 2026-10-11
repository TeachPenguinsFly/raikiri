#pragma once

// poseidon2 over goldilocks and schnorr over ecgfp5; every stage is pinned to the reference signer in the tests

#include <raikiri/config/secret.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::lighter {

inline constexpr std::size_t k_digest_bytes = 40;
inline constexpr std::size_t k_signature_bytes = 80;

// a quintic extension element, five little-endian canonical limbs
using Digest = std::array<std::uint8_t, k_digest_bytes>;
// s then e, each a little-endian scalar
using Signature = std::array<std::uint8_t, k_signature_bytes>;

// limbs need not be canonical: the venue casts negative fields straight to u64
[[nodiscard]] Digest hash_to_quintic(std::span<const std::uint64_t> elements) noexcept;

// the encoded point k * G; exposed for the vectors
[[nodiscard]] Digest encode_base_mul(std::span<const std::uint8_t, k_digest_bytes> scalar) noexcept;

// schnorr with a nonce given; the venue's own signer draws it at random
[[nodiscard]] Signature sign_with_nonce(std::span<const std::uint8_t, k_digest_bytes> secret,
                                        const Digest& digest,
                                        std::span<const std::uint8_t, k_digest_bytes> nonce) noexcept;

// an api key: 40 bytes of hex, as the venue's key generator prints them
class ApiKey {
public:
    ApiKey() = default;
    ApiKey(const ApiKey&) = delete;
    ApiKey& operator=(const ApiKey&) = delete;

    // a value at or above the group order is reduced, as the venue does; zero is refused
    [[nodiscard]] bool load(std::string_view hex_key) noexcept;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] const Digest& public_key() const noexcept { return public_; }

    // the nonce is sha-512 of the key and the digest, so a digest signs to one signature
    [[nodiscard]] Signature sign(const Digest& digest) const noexcept;

private:
    config::Secret key_;
    Digest public_{};
    bool loaded_{false};
};

} // namespace raikiri::venues::lighter
