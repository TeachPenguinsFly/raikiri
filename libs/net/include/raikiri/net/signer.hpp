#pragma once

// request signing; allocation-free and non-throwing

#include <raikiri/config/secret.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::net {

inline constexpr std::size_t k_hmac_bytes = 32;
inline constexpr std::size_t k_hex_bytes = k_hmac_bytes * 2; // 64
inline constexpr std::size_t k_base64_bytes = 44;            // 32 bytes -> 44 chars

inline constexpr std::size_t k_hmac512_bytes = 64;
inline constexpr std::size_t k_hex512_bytes = k_hmac512_bytes * 2; // 128

// raw digest of `message` keyed by `key`; false only on an openssl failure
[[nodiscard]] bool hmac_sha256(std::string_view key,
                               std::string_view message,
                               std::span<std::uint8_t, k_hmac_bytes> out) noexcept;

// lowercase hex (binance, bybit, bingx, xt, mexc, gate); `out` holds at least k_hex_bytes
[[nodiscard]] std::string_view
hmac_sha256_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// standard base64 with padding (okx, kucoin); `out` holds at least k_base64_bytes
[[nodiscard]] std::string_view
hmac_sha256_base64(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// sha512 keyed and unkeyed, lowercase hex; gate's signature embeds a plain sha512 of the body
[[nodiscard]] std::string_view sha512_hex(std::string_view message, std::span<char> out) noexcept;

[[nodiscard]] std::string_view
hmac_sha512_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// holds a venue's credentials; the secret never leaves this object
class Signer {
public:
    enum class Encoding : std::uint8_t { Hex, Base64, Hex512 };

    Signer() = default;
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    // `api_key` is not secret (it travels in a header); `secret` is.
    void set_api_key(std::string_view key) noexcept;

    [[nodiscard]] config::Secret& secret() noexcept { return secret_; }

    [[nodiscard]] bool has_credentials() const noexcept {
        return api_key_len_ != 0 && !secret_.empty();
    }

    [[nodiscard]] std::string_view api_key() const noexcept;

    [[nodiscard]] std::string_view
    sign(std::string_view message, Encoding encoding, std::span<char> out) const noexcept;

private:
    static constexpr std::size_t k_max_api_key = 128;

    config::Secret secret_;
    std::array<char, k_max_api_key> api_key_{};
    std::size_t api_key_len_{0};
};

} // namespace raikiri::net
