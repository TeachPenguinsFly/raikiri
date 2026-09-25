#pragma once

// hmac-sha384, which truncated sha-512 does not reproduce
// rest signs /api/v2 + path + nonce + body; ws signs "AUTH" + nonce + nonce

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

inline constexpr std::size_t k_hmac384_bytes = 48;
inline constexpr std::size_t k_hex384_bytes = k_hmac384_bytes * 2; // 96

// lowercase hex; empty on openssl failure or a short buffer
[[nodiscard]] std::string_view
hmac_sha384_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept;

// the payload is "AUTH" + nonce + the same nonce again; no filter is sent
[[nodiscard]] std::string_view
ws_auth_frame(std::string_view api_key, std::string_view secret, std::uint64_t nonce, std::span<char> out) noexcept;

// microsecond nonces must strictly increase per key, so a counter supplies them
class NonceSource {
public:
    // strictly increasing, even if the wall clock goes back
    [[nodiscard]] std::uint64_t next(std::uint64_t wall_ns) noexcept;

private:
    std::uint64_t last_{0};
};

} // namespace raikiri::venues::bitfinex
