#pragma once

// Request signing for venue REST/WS authentication.
//
// Allocation-free: the caller owns the output buffer and the digest is
// written into it. Signing sits between "decide" and "send", so it must not
// touch the allocator — and it must not throw, because a partially-sent
// order is worse than a crash.

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

// Raw digest of `message` keyed by `key`. Returns false only on an OpenSSL
// failure, which in practice means the process is already unhealthy.
[[nodiscard]] bool hmac_sha256(std::string_view key,
                               std::string_view message,
                               std::span<std::uint8_t, k_hmac_bytes> out) noexcept;

// Lowercase hex encoding (binance, bybit, bingx, xt, mexc, gate).
// `out` must be at least k_hex_bytes; returns the written view.
[[nodiscard]] std::string_view
hmac_sha256_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// Standard base64 with padding (okx, kucoin).
// `out` must be at least k_base64_bytes; returns the written view.
[[nodiscard]] std::string_view
hmac_sha256_base64(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// Holds a venue's credentials for the lifetime of a gateway process. The
// secret never leaves this object: signing happens here, and callers get
// only the digest.
class Signer {
public:
    enum class Encoding : std::uint8_t { Hex, Base64 };

    Signer() = default;
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    // `api_key` is not secret (it travels in a header); `secret` is.
    void set_api_key(std::string_view key) noexcept;

    [[nodiscard]] config::Secret& secret() noexcept { return secret_; }

    // Enough to refuse building a signed request before touching the key
    // material — callers must not have to reach into the secret to ask.
    [[nodiscard]] bool has_credentials() const noexcept {
        return api_key_len_ != 0 && !secret_.empty();
    }

    [[nodiscard]] std::string_view api_key() const noexcept;

    // Signs into the caller's buffer. Empty return means the buffer was too
    // small or OpenSSL failed.
    [[nodiscard]] std::string_view
    sign(std::string_view message, Encoding encoding, std::span<char> out) const noexcept;

private:
    static constexpr std::size_t k_max_api_key = 128;

    config::Secret secret_;
    std::array<char, k_max_api_key> api_key_{};
    std::size_t api_key_len_{0};
};

} // namespace raikiri::net
